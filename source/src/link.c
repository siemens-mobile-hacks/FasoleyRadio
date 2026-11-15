/* Stream connection: TCP or TLS over it (see link.h). Helper context. */
#ifdef HOST_TEST
#include "link_host.h"
#else
#include <swilib.h>
#define lk_alloc(n)	malloc(n)
#define lk_free(p)	mfree(p)
#endif
#include "bearssl.h"
#include "net.h"
#include "link.h"

/* Server certificate: no trust anchors on the phone, so nothing in the chain could be verified anyway; the
 * leaf's public key is taken as it is (br_x509_decoder: one certificate, no signature checks). That avoids
 * the X.509 chain engine and the P-384/SHA-384 code that ECDSA chains (Let's Encrypt E5/E6) would need. */
typedef struct {
	const br_x509_class *vtable;
	br_x509_decoder_context dec;
	int n;			/* certificates seen */
} X509_LEAF;

static void leaf_start_chain(const br_x509_class **ctx, const char *server_name)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	(void)server_name;
	x->n = 0;
}

static void leaf_start_cert(const br_x509_class **ctx, uint32_t length)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	(void)length;
	if (x->n == 0)
		br_x509_decoder_init(&x->dec, 0, 0);
}

static void leaf_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	if (x->n == 0)
		br_x509_decoder_push(&x->dec, buf, len);
}

static void leaf_end_cert(const br_x509_class **ctx)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	x->n++;
}

static unsigned leaf_end_chain(const br_x509_class **ctx)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	if (!x->n)
		return BR_ERR_X509_EMPTY_CHAIN;
	return br_x509_decoder_get_pkey(&x->dec) ? 0 : (unsigned)br_x509_decoder_last_error(&x->dec);
}

static const br_x509_pkey *leaf_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
	X509_LEAF *x = (X509_LEAF *)(void *)ctx;
	if (usages)
		*usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
	return br_x509_decoder_get_pkey(&x->dec);
}

static const br_x509_class leaf_vtable = {
	sizeof(X509_LEAF), leaf_start_chain, leaf_start_cert, leaf_append, leaf_end_cert, leaf_end_chain, leaf_get_pkey
};

typedef struct {
	br_ssl_client_context cc;
	X509_LEAF xc;
	unsigned char io[BR_SSL_BUFSIZE_MONO];	/* one record in or out; servers (Go/Caddy) ignore max_fragment_length */
} TLS;

static TLS *t;
static int tls_on, up;
const char *link_err;
int link_tls_err;

unsigned link_tls_mem(void)
{
	return sizeof(TLS);
}

/* TLS 1.2, ECDHE on P-256 only, RSA or ECDSA server key, ChaCha20-Poly1305 or AES-128-GCM, SHA-256:
 * what Caddy/Go, nginx, Icecast with OpenSSL all accept, with as little code as possible (the elf's code
 * is RAM on SG). */
static void client_init(br_ssl_client_context *cc)
{
	static const uint16_t suites[] = {
		BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
		BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
		BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
		BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	};

	br_ssl_client_zero(cc);
	br_ssl_engine_set_versions(&cc->eng, BR_TLS12, BR_TLS12);
	br_ssl_engine_set_suites(&cc->eng, suites, sizeof(suites) / sizeof(suites[0]));
	br_ssl_engine_set_ec(&cc->eng, &br_ec_p256_m31);
	br_ssl_engine_set_rsavrfy(&cc->eng, &br_rsa_i31_pkcs1_vrfy);
	br_ssl_engine_set_ecdsa(&cc->eng, &br_ecdsa_i31_vrfy_asn1);
	br_ssl_engine_set_hash(&cc->eng, br_sha256_ID, &br_sha256_vtable);
	br_ssl_engine_set_hash(&cc->eng, br_sha224_ID, &br_sha224_vtable);
	br_ssl_engine_set_hash(&cc->eng, br_sha1_ID, &br_sha1_vtable);	/* old servers sign the key exchange with it */
	br_ssl_engine_set_prf_sha256(&cc->eng, &br_tls12_sha256_prf);
	br_ssl_engine_set_default_aes_gcm(&cc->eng);
	br_ssl_engine_set_default_chapol(&cc->eng);
}

static void entropy(unsigned char *buf, int len)
{
#ifdef HOST_TEST
	lk_entropy(buf, len);
#else
	/* weak (uptime, heap state, addresses), but nothing here is checked anyway (no trust anchors) */
	uint32_t *o = (uint32_t *)buf, x = GetSessionAge() * 2654435761u ^ GetFreeRamAvail() ^ (uint32_t)&o;
	void *m = malloc(16);
	int i;
	x ^= (uint32_t)m << 3;
	mfree(m);
	for (i = 0; i < len / 4; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		o[i] = x ^ (uint32_t)GetSessionAge() << (i & 15);
	}
#endif
}

static void fail(int err)
{
	static char b[48];
	link_tls_err = err;
	if (err) {
		int n = 0, v = err, k;
		char d[8];
		const char *p = "TLS error ";
		while (*p)
			b[n++] = *p++;
		k = 0;
		do {
			d[k++] = (char)('0' + v % 10);
			v /= 10;
		} while (v && k < 7);
		while (k)
			b[n++] = d[--k];
		b[n] = 0;
		link_err = b;
	} else
		link_err = "TLS error";
}

void link_close(void)
{
	net_close();
	if (t) {
		lk_free(t);
		t = 0;
	}
	tls_on = 0;
	up = 0;
}

void link_open(const char *host, int port, int tls)
{
	link_close();
	link_err = 0;
	link_tls_err = 0;
	tls_on = tls;
	if (tls) {
		unsigned char seed[32];
		t = lk_alloc(sizeof(TLS));
		if (!t) {
			link_err = "Not enough memory for TLS";
			return;
		}
		client_init(&t->cc);
		t->xc.vtable = &leaf_vtable;
		br_ssl_engine_set_x509(&t->cc.eng, &t->xc.vtable);
		br_ssl_engine_set_buffer(&t->cc.eng, t->io, sizeof(t->io), 0);
		entropy(seed, sizeof(seed));
		br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof(seed));
		if (!br_ssl_client_reset(&t->cc, host, 0)) {
			fail(br_ssl_engine_last_error(&t->cc.eng));
			return;
		}
	}
	net_open(host, port);
}

/* records between the engine and the socket; -1 = TLS closed/failed */
static int pump(void)
{
	br_ssl_engine_context *e = &t->cc.eng;
	int loops;

	for (loops = 0; loops < 32; loops++) {
		unsigned s = br_ssl_engine_current_state(e);
		size_t len;
		unsigned char *buf;
		int n;

		if (s & BR_SSL_CLOSED) {
			int err = br_ssl_engine_last_error(e);
			if (err || !up)
				fail(err);
			return -1;
		}
		if (s & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))
			up = 1;
		if (s & BR_SSL_SENDREC) {
			buf = br_ssl_engine_sendrec_buf(e, &len);
			n = net_send(buf, (int)len);
			if (n < 0)
				return -1;
			if (n == 0)
				return 0;
			br_ssl_engine_sendrec_ack(e, (size_t)n);
			continue;
		}
		if (s & BR_SSL_RECVREC) {
			buf = br_ssl_engine_recvrec_buf(e, &len);
			n = net_recv(buf, (int)len);
			if (n < 0)
				return -1;
			if (n == 0)
				return 0;
			br_ssl_engine_recvrec_ack(e, (size_t)n);
			continue;
		}
		return 0;
	}
	return 0;
}

int link_state(void)
{
	int ns = net_state();

	if (!tls_on)
		return ns;
	if (!t || link_err)
		return NET_FAILED;
	if (ns != NET_UP)
		return ns;
	if (pump() < 0)
		return link_err ? NET_FAILED : NET_CLOSED;
	return up ? NET_UP : NET_CONNECTING;
}

int link_send(const void *buf, int len)
{
	br_ssl_engine_context *e;
	size_t room;
	unsigned char *b;
	int n;

	if (!tls_on)
		return net_send(buf, len);
	if (!t || link_err)
		return -1;
	e = &t->cc.eng;
	if (pump() < 0)
		return -1;
	if (!(br_ssl_engine_current_state(e) & BR_SSL_SENDAPP))
		return 0;
	b = br_ssl_engine_sendapp_buf(e, &room);
	n = len < (int)room ? len : (int)room;
	for (int i = 0; i < n; i++)
		b[i] = ((const unsigned char *)buf)[i];
	br_ssl_engine_sendapp_ack(e, (size_t)n);
	br_ssl_engine_flush(e, 0);	/* the request is one small record */
	if (pump() < 0)
		return -1;
	return n;
}

int link_recv(void *buf, int len)
{
	br_ssl_engine_context *e;
	size_t have;
	unsigned char *b;
	int n;

	if (!tls_on)
		return net_recv(buf, len);
	if (!t || link_err)
		return -1;
	e = &t->cc.eng;
	if (!(br_ssl_engine_current_state(e) & BR_SSL_RECVAPP) && pump() < 0)
		return -1;
	if (!(br_ssl_engine_current_state(e) & BR_SSL_RECVAPP))
		return 0;
	b = br_ssl_engine_recvapp_buf(e, &have);
	n = len < (int)have ? len : (int)have;
	for (int i = 0; i < n; i++)
		((unsigned char *)buf)[i] = b[i];
	br_ssl_engine_recvapp_ack(e, (size_t)n);
	return n;
}
