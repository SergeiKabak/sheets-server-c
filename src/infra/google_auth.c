#include "infra/google_auth.h"

#include "core/json_text.h"
#include "core/num.h"
#include "core/url_codec.h"
#include "infra/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GOOGLE_TOKEN_FILE "/tmp/trk_sa_token"
#define GOOGLE_TMPFMT    "/tmp/trk_%d_%s"

const char *google_auth_cache_path(void) {
    const char *p = getenv("TRK_TOKEN_CACHE");
    return (p && *p) ? p : GOOGLE_TOKEN_FILE;
}

/* Уникальное имя временного файла (с PID): запросы обрабатываются в
   отдельных процессах, общие файлы давали гонку. */
static const char *tmp_file(const char *suffix, char *buf, size_t cap) {
    snprintf(buf, cap, GOOGLE_TMPFMT, (int)getpid(), suffix);
    return buf;
}

void google_auth_init(GoogleAuth *auth, const char *sa_json) {
    if (!auth) return;
    auth->token = NULL;
    auth->token_exp = 0;
    snprintf(auth->token_cache_path, sizeof(auth->token_cache_path), "%s",
             google_auth_cache_path());
    const char *env = sa_json ? sa_json : getenv("GOOGLE_SERVICE_ACCOUNT_JSON");
    auth->sa_json = env ? strdup(env) : NULL;
}

void google_auth_reset(GoogleAuth *auth) {
    if (!auth) return;
    free(auth->token);
    auth->token = NULL;
    auth->token_exp = 0;
}

/* ---------- base64url (без padding) ---------- */
static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *b64url(const unsigned char *in, size_t len) {
    size_t olen = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(olen);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 0x3F];
        out[o++] = B64URL[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64URL[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64URL[v & 0x3F] : '=';
        i += 3;
    }
    out[o] = '\0';
    while (o && out[o - 1] == '=') out[--o] = '\0';
    return out;
}

/* Читает токен из файлового кэша. 1 — токен прочитан, 0 — кэша нет. */
static int token_from_cache_file(GoogleAuth *auth) {
    FILE *tf = fopen(auth->token_cache_path, "r");
    if (!tf) return 0;
    char expl[32] = "", tok[8192] = "";
    long fexp = 0;
    if (fgets(expl, sizeof(expl), tf)) fexp = strtol(expl, NULL, 10);
    if (fgets(tok, sizeof(tok), tf)) {
        char *nl = strchr(tok, '\n');
        if (nl) *nl = '\0';
    }
    fclose(tf);
    if (!tok[0] || fexp <= time(NULL) + 60) return 0;

    free(auth->token);
    auth->token = strdup(tok);
    auth->token_exp = fexp;
    return auth->token != NULL;
}

/* Кладёт токен в файл (атомарно, права 600 — читатели не увидят
   частичную запись). */
static void token_to_cache_file(GoogleAuth *auth) {
    char tmp[GOOGLE_TOKEN_CACHE_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", auth->token_cache_path, (int)getpid());
    FILE *tf = fopen(tmp, "w");
    if (tf) {
        fprintf(tf, "%ld\n%s\n", (long)auth->token_exp, auth->token);
        fclose(tf);
        chmod(tmp, 0600);
        if (rename(tmp, auth->token_cache_path) != 0) remove(tmp);
    } else {
        remove(tmp);
    }
}

const char *google_auth_token(GoogleAuth *auth) {
    if (!auth) return NULL;
    if (auth->token && time(NULL) < auth->token_exp - 60) return auth->token;

    if (token_from_cache_file(auth)) return auth->token;

    if (!auth->sa_json) return NULL;

    char *email = json_field_string(auth->sa_json, "client_email");
    char *pkey  = json_field_string(auth->sa_json, "private_key");
    char *turi  = json_field_string(auth->sa_json, "token_uri");
    if (!email || !pkey) {
        fprintf(stderr, "[track] неверный JSON сервисного аккаунта: "
                        "client_email=%s private_key=%s\n",
                email ? "есть" : "НЕТ", pkey ? "есть" : "НЕТ");
        free(email); free(pkey); free(turi);
        return NULL;
    }
    if (!turi) turi = strdup("https://oauth2.googleapis.com/token");

    /* приватный ключ в PEM-файл (в JSON переносы строк как \n) */
    char keyfile[256];
    tmp_file("sa_key.pem", keyfile, sizeof(keyfile));
    FILE *kp = fopen(keyfile, "w");
    if (!kp) { free(email); free(pkey); free(turi); return NULL; }
    fputs(pkey, kp);
    fclose(kp);
    chmod(keyfile, 0600);

    /* JWT header + payload */
    char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    char payload[1024];
    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
        "{\"iss\":\"%s\",\"scope\":\"https://www.googleapis.com/auth/spreadsheets\","
        "\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":%ld,\"exp\":%ld}",
        email, (long)now, (long)(now + 3600));

    char *h = b64url((const unsigned char *)header, strlen(header));
    char *p = b64url((const unsigned char *)payload, strlen(payload));
    if (!h || !p) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }

    char signing[2048];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);

    char infile[256], sigfile[256];
    tmp_file("jwt_in.txt", infile, sizeof(infile));
    tmp_file("jwt_sig.bin", sigfile, sizeof(sigfile));
    FILE *in = fopen(infile, "w");
    if (!in) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }
    fputs(signing, in);
    fclose(in);

    /* подпись RSA-SHA256 */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "openssl dgst -sha256 -sign '%s' -out '%s' '%s' 2>&1",
             keyfile, sigfile, infile);
    if (system(cmd) != 0) {
        fprintf(stderr, "[track] openssl signing failed\n");
        free(email); free(pkey); free(turi); free(h); free(p);
        return NULL;
    }

    FILE *sf = fopen(sigfile, "rb");
    if (!sf) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }
    unsigned char sig[1024];
    size_t siglen = fread(sig, 1, sizeof(sig), sf);
    fclose(sf);

    remove(infile);
    remove(sigfile);
    remove(keyfile);
    char *sig_b64 = b64url(sig, siglen);
    if (!sig_b64) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }

    char jwt[3072];
    snprintf(jwt, sizeof(jwt), "%s.%s", signing, sig_b64);

    char body[4096];
    snprintf(body, sizeof(body),
        "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Ajwt-bearer"
        "&assertion=%s", jwt);

    int status = 0;
    char *resp = http_request_full("POST", turi, "application/x-www-form-urlencoded",
                                   body, NULL, &status);
    free(email); free(pkey); free(turi); free(h); free(p); free(sig_b64);

    if (!resp || status != 200) {
        fprintf(stderr, "[track] не удалось получить токен доступа: код %d, ответ %.180s\n",
                status, resp ? resp : "(нет ответа)");
        free(resp);
        return NULL;
    }
    char *tok = json_field_string(resp, "access_token");
    /* срок жизни читаем ДО освобождения ответа (иначе use-after-free) */
    long exp = 3600;
    char *e = json_field_string(resp, "expires_in");
    if (e) { long v = atol(e); if (v > 60) exp = v; free(e); }
    free(resp);
    if (!tok) {
        fprintf(stderr, "[track] в ответе Google нет access_token\n");
        return NULL;
    }

    free(auth->token);
    auth->token = tok;
    auth->token_exp = now + exp - 120;
    fprintf(stderr, "[track] access token obtained\n");

    token_to_cache_file(auth);
    return auth->token;
}
