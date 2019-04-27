/*
 * Copyright (C) 2019 Nicola Di Lieto <nicola.dilieto@gmail.com>
 *
 * This file is part of uacme.
 *
 * uacme is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * uacme is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "base64.h"
#include "curlwrap.h"
#include "crypto.h"
#include "json.h"
#include "msg.h"

#define PRODUCTION_URL "https://acme-v02.api.letsencrypt.org/directory"
#define STAGING_URL "https://acme-staging-v02.api.letsencrypt.org/directory"
#define DEFAULT_CONFDIR "/etc/ssl/uacme"

typedef struct acme
{
    privkey_t key;
    privkey_t dkey;
    json_value_t *json;
    json_value_t *account;
    json_value_t *dir;
    json_value_t *order;
    char *nonce;
    char *kid;
    char *headers;
    char *body;
    char *type;
    const char *directory;
    const char *hook;
    const char *email;
    const char *domain;
    const char * const *names;
    const char *confdir;
    char *keydir;
    char *dkeydir;
    char *certdir;
} acme_t;

char *find_header(const char *headers, const char *name)
{
    char *regex = NULL;
    if (asprintf(&regex, "^%s: (.*)\r\n", name) < 0)
    {
        return NULL;
    }
    char *ret = NULL;
    regex_t reg;
    if (regcomp(&reg, regex, REG_EXTENDED | REG_NEWLINE))
    {
        warnx("find_header: regcomp failed");
    }
    else
    {
        regmatch_t m[2];
        if (regexec(&reg, headers, 2, m, 0) == 0)
        {
            ret = strndup(headers + m[1].rm_so, m[1].rm_eo - m[1].rm_so);
            if (!ret)
            {
                warn("find_header: strndup failed");
            }
        }
    }
    free(regex);
    regfree(&reg);
    return ret;
}

int acme_get(acme_t *a, const char *url)
{
    int ret = 0;
    curldata_t *c = NULL;

    json_free(a->json);
    a->json = NULL;
    free(a->headers);
    a->headers = NULL;
    free(a->body);
    a->body = NULL;
    free(a->type);
    a->type = NULL;

    if (!url)
    {
        warnx("acme_get: invalid URL");
        goto out;
    }
    if (g_loglevel > 1)
    {
        warnx("acme_get: url=%s", url);
    }
    c = curl_get(url);
    if (!c)
    {
        warnx("acme_get: curl_get failed");
        goto out;
    }
    free(a->nonce);
    a->nonce = find_header(c->headers, "Replay-Nonce");
    a->type = find_header(c->headers, "Content-Type");
    if (a->type && strstr(a->type, "json"))
    {
        a->json = json_parse(c->body, c->body_len);
    }
    a->headers = c->headers;
    c->headers = NULL;
    a->body = c->body;
    c->body = NULL;
    ret = c->code;
out:
    curldata_free(c);
    if (g_loglevel > 2)
    {
        if (a->headers)
        {
            warnx("acme_get: HTTP headers\n%s", a->headers);
        }
        if (a->body)
        {
            warnx("acme_get: HTTP body\n%s", a->body);
        }
    }
    if (!a->headers) a->headers = strdup("");
    if (!a->body) a->body = strdup("");
    if (!a->type) a->type = strdup("");
    return ret;
}

int acme_post(acme_t *a, const char *url, const char *format, ...)
{
    int ret = 0;
    char *payload = NULL;
    char *protected = NULL;
    char *jws = NULL;
    curldata_t *c = NULL;

    json_free(a->json);
    a->json = NULL;
    free(a->headers);
    a->headers = NULL;
    free(a->body);
    a->body = NULL;
    free(a->type);
    a->type = NULL;

    if (!a->nonce)
    {
        warnx("acme_post: need a nonce first");
        goto out;
    }

    va_list ap;
    va_start(ap, format);
    if (vasprintf(&payload, format, ap) < 0)
    {
        warnx("acme_post: vasprintf failed");
        payload = NULL;
    }
    va_end(ap);
    if (!payload) return 0;

    if (!url)
    {
        warnx("acme_post: invalid URL");
        goto out;
    }
    protected = (a->kid && *a->kid) ?
        jws_protected_kid(a->nonce, url, a->kid) :
        jws_protected_jwk(a->nonce, url, a->key);
    if (!protected)
    {
        warnx("acme_post: jws_protected_xxx failed");
        goto out;
    }
    jws = jws_encode(protected, payload, a->key);
    if (!jws)
    {
        warnx("acme_post: jws_encode failed");
        goto out;
    }
    if (g_loglevel > 2)
    {
        warnx("acme_post: url=%s payload=%s nonce=%s request=%s",
                url, payload, a->nonce, jws);
    }
    else if (g_loglevel > 1)
    {
        warnx("acme_post: url=%s payload=%s", url, payload);
    }
    c = curl_post(url, jws);
    if (!c)
    {
        warnx("acme_post: curl_post failed");
        goto out;
    }
    free(a->nonce);
    a->nonce = find_header(c->headers, "Replay-Nonce");
    a->type = find_header(c->headers, "Content-Type");
    if (a->type && strstr(a->type, "json"))
    {
        a->json = json_parse(c->body, c->body_len);
    }
    a->headers = c->headers;
    c->headers = NULL;
    a->body = c->body;
    c->body = NULL;
    ret = c->code;
out:
    free(payload);
    free(protected);
    free(jws);
    curldata_free(c);
    if (g_loglevel > 2)
    {
        if (a->headers)
        {
            warnx("acme_post: HTTP headers:\n%s", a->headers);
        }
        if (a->body)
        {
            warnx("acme_post: HTTP body:\n%s", a->body);
        }
    }
    if (g_loglevel > 1)
    {
        warnx("acme_post: return code %d, json=", ret);
        if (a->json)
        {
            json_dump(stderr, a->json);
        }
        else
        {
            fprintf(stderr, "<null>\n");
        }
    }
    if (!a->headers) a->headers = strdup("");
    if (!a->body) a->body = strdup("");
    if (!a->type) a->type = strdup("");
    return ret;
}

int hook_run(const char *prog, const char *method, const char *type,
        const char *ident, const char *token, const char *auth)
{
    int ret = -1;
    pid_t pid = fork();
    if (pid < 0)
    {
        warn("hook_run: fork failed");
    }
    else if (pid > 0) // parent
    {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            ret = WEXITSTATUS(status);
        }
    }
    else // child
    {
        if (execl(prog, prog, method, type, ident, token, auth,
                    (char *)NULL) < 0)
        {
            warn("hook_run: failed to execute %s", prog);
            abort();
        }
    }
    return ret;
}

bool check_or_mkdir(bool allow_create, const char *dir, mode_t mode)
{
    if (access(dir, F_OK) < 0)
    {
        if (!allow_create)
        {
            warnx("failed to access %s", dir);
            return false;
        }
        if (mkdir(dir, mode) < 0)
        {
            warn("failed to create %s", dir);
            return false;
        }
        msg(1, "created directory %s", dir);
    }
    struct stat st;
    if (stat(dir, &st) != 0)
    {
        warn("failed to stat %s", dir);
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        warnx("%s is not a directory", dir);
        return false;
    }
    return true;
}

char *identifiers(const char * const *names)
{
    char *ids = NULL;
    char *tmp = NULL;
    if (asprintf(&tmp, "{\"identifiers\":[") < 0)
    {
        warnx("identifiers: asprintf failed");
        return NULL;
    }
    while (names && *names)
    {
        if (asprintf(&ids, "%s{\"type\":\"dns\",\"value\":\"%s\"},",
                    tmp, *(names++)) < 0)
        {
            warnx("identifiers: asprintf failed");
            free(tmp);
            return NULL;
        }
        free(tmp);
        tmp = ids;
        ids = NULL;
    }
    tmp[strlen(tmp)-1] = 0;
    if (asprintf(&ids, "%s]}", tmp) < 0)
    {
        warnx("identifiers: asprintf failed");
        ids = NULL;
    }
    free(tmp);
    return ids;
}

bool acme_error(acme_t *a)
{
    if (!a->json) return false;

    if (a->type && strcasecmp(a->type,
                "application/problem+json") == 0)
    {
        warnx("the server reported the following error:");
        json_dump(stderr, a->json);
        return true;
    }

    const json_value_t *e = json_find(a->json, "error");
    if (e && e->type == JSON_OBJECT)
    {
        warnx("the server reported the following error:");
        json_dump(stderr, e);
        return true;
    }

    return false;
}

bool acme_bootstrap(acme_t *a)
{
    msg(1, "fetching directory at %s", a->directory);
    if (200 != acme_get(a, a->directory))
    {
        warnx("failed to fetch directory at %s", a->directory);
        acme_error(a);
        return false;
    }
    else if (acme_error(a))
    {
        return false;
    }
    a->dir = a->json;
    a->json = NULL;

    const char *url = json_find_string(a->dir, "newNonce");
    if (!url)
    {
        warnx("failed to find newNonce URL in directory");
        return false;
    }

    msg(2, "fetching new nonce at %s", url);
    if (204 != acme_get(a, url))
    {
        warnx("failed to fetch new nonce at %s", url);
        acme_error(a);
        return false;
    }
    else if (acme_error(a))
    {
        return false;
    }
    return true;
}

bool account_new(acme_t *a, bool yes)
{
    const char *url = json_find_string(a->dir, "newAccount");
    if (!url)
    {
        warnx("failed to find newAccount URL in directory");
        return false;
    }

    msg(1, "creating new account at %s", url);
    switch (acme_post(a, url, "{\"onlyReturnExisting\":true}"))
    {
        case 200:
            if (!(a->kid = find_header(a->headers, "Location")))
            {
                warnx("account exists but location not found");
                return false;
            }
            warnx("Account already exists at %s", a->kid);
            return false;

        case 400:
            if (a->json && a->type &&
                    0 == strcasecmp(a->type, "application/problem+json") &&
                    0 == json_compare_string(a->json, "type",
                        "urn:ietf:params:acme:error:accountDoesNotExist"))
            {
                const json_value_t *meta = json_find(a->dir, "meta");
                const char *terms = json_find_string(meta, "termsOfService");
                if (terms)
                {
                    if (yes)
                    {
                        msg(0, "terms at %s autoaccepted (-y)", terms);
                    }
                    else
                    {
                        char c = 0;
                        msg(0, "type 'y' to accept the terms at %s", terms);
                        if (scanf(" %c", &c) != 1 || tolower(c) != 'y')
                        {
                            warnx("terms not agreed to, aborted");
                            return false;
                        }
                    }
                }
                int r = 0;
                if (a->email && strlen(a->email))
                {
                    r = acme_post(a, url, "{\"termsOfServiceAgreed\":true"
                                ",\"contact\": [\"mailto:%s\"]}", a->email);
                }
                else
                {
                    r = acme_post(a, url, "{\"termsOfServiceAgreed\":true}");
                }
                if (r == 201)
                {
                    if (acme_error(a))
                    {
                        return false;
                    }
                    if (json_compare_string(a->json, "status", "valid"))
                    {
                        const char* status = json_find_string(a->json, "status");
                        warnx("account created but status is not valid (%s)",
                                status ? status : "unknown");
                        return false;
                    }
                    if (!(a->kid = find_header(a->headers, "Location")))
                    {
                        warnx("account created but location not found");
                        return false;
                    }
                    msg(1, "account created at %s", a->kid);
                    return true;
                }
            }
            // intentional fallthrough
        default:
            warnx("failed to create account at %s", url);
            acme_error(a);
            return false;
    }
}

bool account_retrieve(acme_t *a)
{
    const char *url = json_find_string(a->dir, "newAccount");
    if (!url)
    {
        warnx("failed to find newAccount URL in directory");
        return false;
    }
    msg(1, "retrieving account at %s", url);
    switch (acme_post(a, url, "{\"onlyReturnExisting\":true}"))
    {
        case 200:
            if (acme_error(a))
            {
                return false;
            }
            break;

        case 400:
            if (a->json && a->type &&
                    0 == strcasecmp(a->type, "application/problem+json") &&
                    0 == json_compare_string(a->json, "type",
                        "urn:ietf:params:acme:error:accountDoesNotExist"))
            {
                warnx("no account associated with %s/key.pem found at %s. "
                        "Consider trying 'new'", a->keydir, url);
                return false;
            }
            // intentional fallthrough
        default:
            warnx("failed to retrieve account at %s", url);
            acme_error(a);
            return false;
    }
    if (json_compare_string(a->json, "status", "valid"))
    {
        const char* status = json_find_string(a->json, "status");
        warnx("invalid account status (%s)", status ? status : "unknown");
        return false;
    }
    if (!(a->kid = find_header(a->headers, "Location")))
    {
        warnx("account location not found");
        return false;
    }
    msg(1, "account location: %s", a->kid);
    a->account = a->json;
    a->json = NULL;
    return true;
}

bool account_update(acme_t *a)
{
    bool email_update = false;
    const json_value_t *contacts = json_find(a->account, "contact");
    if (contacts && contacts->type != JSON_ARRAY)
    {
        warnx("failed to parse account contacts");
        return false;
    }
    if (a->email && strlen(a->email) > 0)
    {
        if (!contacts || contacts->v.array.size == 0)
        {
            email_update = true;
        }
        else for (int i=0; i<contacts->v.array.size; i++)
        {
            if (contacts->v.array.values[i].type != JSON_STRING ||
                    contacts->v.array.values[i].v.value !=
                    strcasestr(contacts->v.array.values[i].v.value,
                        "mailto:"))
            {
                warnx("failed to parse account contacts");
                return false;
            }
            if (strcasecmp(contacts->v.array.values[i].v.value
                        + strlen("mailto:"), a->email))
            {
                email_update = true;
            }
        }
    }
    else if (contacts && contacts->v.array.size > 0)
    {
        email_update = true;
    }
    if (email_update)
    {
        int ret = 0;
        if (a->email && strlen(a->email) > 0)
        {
            msg(1, "updating account email to %s at %s", a->email, a->kid);
            ret = acme_post(a, a->kid, "{\"contact\": [\"mailto:%s\"]}",
                    a->email);
        }
        else
        {
            msg(1, "removing account email at %s", a->kid);
            ret = acme_post(a, a->kid, "{\"contact\": []}");
        }
        if (ret != 200)
        {
            warnx("failed to update account email at %s", a->kid);
            acme_error(a);
            return false;
        }
        else if (acme_error(a))
        {
            return false;
        }
        msg(1, "account at %s updated", a->kid);
    }
    else
    {
        msg(1, "email is already up to date for account at %s", a->kid);
    }
    return true;
}

bool account_deactivate(acme_t *a)
{
    msg(1, "deactivating account at %s", a->kid);
    if (200 != acme_post(a, a->kid, "{\"status\": \"deactivated\"}"))
    {
        warnx("failed to deactivate account at %s", a->kid);
        acme_error(a);
        return false;
    }
    else if (acme_error(a))
    {
        return false;
    }
    msg(1, "account at %s deactivated", a->kid);
    return true;
}

bool authorize(acme_t *a)
{
    bool success = false;
    char *thumbprint = NULL;
    json_value_t *auth = NULL;
    const json_value_t *auths = json_find(a->order, "authorizations");
    if (!auths || auths->type != JSON_ARRAY)
    {
        warnx("failed to parse authorizations URL");
        goto out;
    }

    thumbprint = jws_thumbprint(a->key);
    if (!thumbprint)
    {
        goto out;
    }

    for (int i=0; i<auths->v.array.size; i++)
    {
        if (auths->v.array.values[i].type != JSON_STRING)
        {
            warnx("failed to parse authorizations URL");
            goto out;
        }
        msg(1, "retrieving authorization at %s",
                auths->v.array.values[i].v.value);
        if (200 != acme_post(a, auths->v.array.values[i].v.value, ""))
        {
            warnx("failed to retrieve auth %s",
                    auths->v.array.values[i].v.value);
            acme_error(a);
            goto out;
        }
        const char *status = json_find_string(a->json, "status");
        if (status && strcmp(status, "valid") == 0)
        {
            continue;
        }
        if (!status || strcmp(status, "pending") != 0)
        {
            warnx("unexpected auth status (%s) at %s",
                status ? status : "unknown",
                auths->v.array.values[i].v.value);
            acme_error(a);
            goto out;
        }
        const json_value_t *ident = json_find(a->json, "identifier");
        if (json_compare_string(ident, "type", "dns") != 0)
        {
            warnx("no valid identifier in auth %s",
                    auths->v.array.values[i].v.value);
            goto out;
        }
        const char *ident_value = json_find_string(ident, "value");
        if (!ident_value || strlen(ident_value) <= 0)
        {
            warnx("no valid identifier in auth %s",
                    auths->v.array.values[i].v.value);
            goto out;
        }
        const json_value_t *chlgs = json_find(a->json, "challenges");
        if (!chlgs || chlgs->type != JSON_ARRAY)
        {
            warnx("no challenges in auth %s",
                    auths->v.array.values[i].v.value);
            goto out;
        }
        json_free(auth);
        auth = a->json;
        a->json = NULL;

        bool chlg_done = false;
        for (int j=0; j<chlgs->v.array.size && !chlg_done; j++)
        {
            if (json_compare_string(chlgs->v.array.values+j,
                        "status", "pending") == 0)
            {
                const char *url = json_find_string(
                        chlgs->v.array.values+j, "url");
                const char *type = json_find_string(
                        chlgs->v.array.values+j, "type");
                const char *token = json_find_string(
                        chlgs->v.array.values+j, "token");
                char *key_auth = NULL;
                if (!type || !url || !token)
                {
                    warnx("failed to parse challenge");
                    goto out;
                }
                if (strcmp(type, "dns-01") == 0)
                {
                    key_auth = sha256_base64url("%s.%s", token, thumbprint);
                }
                else if (asprintf(&key_auth, "%s.%s", token, thumbprint) < 0)
                {
                    key_auth = NULL;
                }
                if (!key_auth)
                {
                    warnx("failed to generate authorization key");
                    goto out;
                }
                if (a->hook && strlen(a->hook) > 0)
                {
                    msg(2, "type=%s", type);
                    msg(2, "ident=%s", ident_value);
                    msg(2, "token=%s", token);
                    msg(2, "key_auth=%s", key_auth);
                    msg(1, "running %s %s %s %s %s %s", a->hook, "begin",
                            type, ident_value, token, key_auth);
                    int r = hook_run(a->hook, "begin", type, ident_value, token,
                            key_auth);
                    msg(2, "hook returned %d", r);
                    if (r < 0)
                    {
                        free(key_auth);
                        goto out;
                    }
                    else if (r > 0)
                    {
                        msg(1, "challenge %s declined", type);
                        free(key_auth);
                        continue;
                    }
                }
                else
                {
                    char c = 0;
                    msg(0, "challenge=%s ident=%s token=%s key_auth=%s",
                        type, ident_value, token, key_auth);
                    msg(0, "type 'y' to accept challenge, anything else to skip");
                    if (scanf(" %c", &c) != 1 || tolower(c) != 'y')
                    {
                        free(key_auth);
                        continue;
                    }
                }

                msg(1, "starting challenge at %s", url);
                if (200 != acme_post(a, url, "{}"))
                {
                    warnx("failed to start challenge at %s", url);
                    acme_error(a);
                }
                else while (!chlg_done)
                {
                    msg(1, "polling challenge status at %s", url);
                    if (200 != acme_post(a, url, ""))
                    {
                        warnx("failed to poll challenge status at %s", url);
                        acme_error(a);
                        break;
                    }
                    const char *status = json_find_string(a->json, "status");
                    if (status && strcmp(status, "valid") == 0)
                    {
                        chlg_done = true;
                    }
                    else if (!status || (strcmp(status, "processing") != 0 &&
                            strcmp(status, "pending") != 0))
                    {
                        warnx("challenge %s failed with status %s",
                                url, status ? status : "unknown");
                        acme_error(a);
                        break;
                    }
                    else
                    {
                        msg(2, "challenge %s, waiting 5 seconds", status);
                        sleep(5);
                    }
                }
                if (a->hook && strlen(a->hook) > 0)
                {
                    const char *method = chlg_done ? "done" : "failed";
                    msg(1, "running %s %s %s %s %s %s", a->hook, method,
                            type, ident_value, token, key_auth);
                    hook_run(a->hook, method, type, ident_value, token, key_auth);
                }
                free(key_auth);
                if (!chlg_done)
                {
                    goto out;
                }
            }
        }
        if (!chlg_done)
        {
            warnx("no challenge completed");
            goto out;
        }
    }

    success = true;

out:
    json_free(auth);
    free(thumbprint);
    return success;
}

bool cert_issue(acme_t *a)
{
    bool success = false;
    char *csr = NULL;
    char *orderurl = NULL;
    char *ids = identifiers(a->names);
    if (!ids)
    {
        warnx("failed to process alternate names");
        goto out;
    }

    const char *url = json_find_string(a->dir, "newOrder");
    if (!url)
    {
        warnx("failed to find newOrder URL in directory");
        goto out;
    }

    msg(1, "creating new order for %s at %s", a->domain, url);
    if (201 != acme_post(a, url, ids))
    {
        warnx("failed to create new order at %s", url);
        acme_error(a);
        goto out;
    }
    const char *status = json_find_string(a->json, "status");
    if (!status || (strcmp(status, "pending") && strcmp(status, "ready")))
    {
        warnx("invalid order status (%s)", status ? status : "unknown");
        acme_error(a);
        goto out;
    }
    orderurl = find_header(a->headers, "Location");
    if (!orderurl)
    {
        warnx("order location not found");
        goto out;
    }
    msg(1, "order URL: %s", orderurl);
    a->order = a->json;
    a->json = NULL;

    if (strcmp(status, "ready") != 0)
    {
        if (!authorize(a))
        {
            warnx("failed to authorize order at %s", orderurl);
            goto out;
        }
        while (1)
        {
            msg(1, "polling order status at %s", orderurl);
            if (200 != acme_post(a, orderurl, ""))
            {
                warnx("failed to poll order status at %s", orderurl);
                acme_error(a);
                goto out;
            }
            status = json_find_string(a->json, "status");
            if (status && strcmp(status, "ready") == 0)
            {
                json_free(a->order);
                a->order = a->json;
                a->json = NULL;
                break;
            }
            else if (!status || strcmp(status, "pending") != 0)
            {
                warnx("unexpected order status (%s) at %s",
                        status ? status : "unknown", orderurl);
                acme_error(a);
                goto out;
            }
            else
            {
                msg(2, "order pending, waiting 5 seconds");
                sleep(5);
            }
        }
    }

    msg(1, "generating certificate request");
    csr = csr_gen(a->names, a->dkey);
    if (!csr)
    {
        warnx("failed to generate certificate signing request");
        goto out;
    }

    const char *finalize = json_find_string(a->order, "finalize");
    if (!finalize)
    {
        warnx("failed to find finalize URL");
        goto out;
    }

    msg(1, "finalizing order at %s", finalize);
    if (200 != acme_post(a, finalize, "{\"csr\": \"%s\"}", csr))
    {
        warnx("failed to finalize order at %s", finalize);
        acme_error(a);
        goto out;
    }
    else if (acme_error(a))
    {
        goto out;
    }

    while (1)
    {
        msg(1, "polling order status at %s", orderurl);
        if (200 != acme_post(a, orderurl, ""))
        {
            warnx("failed to poll order status at %s", orderurl);
            acme_error(a);
            goto out;
        }
        status = json_find_string(a->json, "status");
        if (status && strcmp(status, "valid") == 0)
        {
            json_free(a->order);
            a->order = a->json;
            a->json = NULL;
            break;
        }
        else if (!status || strcmp(status, "processing") != 0)
        {
            warnx("unexpected order status (%s) at %s",
                    status ? status : "unknown", orderurl);
            acme_error(a);
            goto out;
        }
        else
        {
            msg(2, "order processing, waiting 5 seconds");
            sleep(5);
        }
    }

    const char *certurl = json_find_string(a->order, "certificate");
    if (!certurl)
    {
        warnx("failed to parse certificate url");
        goto out;
    }

    msg(1, "retrieving certificate at %s", certurl);
    if (200 != acme_post(a, certurl, ""))
    {
        warnx("failed to retrieve certificate at %s", certurl);
        acme_error(a);
        goto out;
    }
    else if (acme_error(a))
    {
        goto out;
    }

    if (!cert_save(a->body, a->certdir))
    {
        warnx("failed to save certificate");
        goto out;
    }

    success = true;

out:
    free(csr);
    free(ids);
    free(orderurl);
    return success;
}

bool cert_revoke(acme_t *a, const char *certfile, int reason_code)
{
    bool success = false;
    const char *url = NULL;
    char *crt = cert_der_base64url(certfile);
    if (!crt)
    {
        warnx("failed to load %s", certfile);
        goto out;
    }

    url = json_find_string(a->dir, "revokeCert");
    if (!url)
    {
        warnx("failed to find revokeCert URL in directory");
        goto out;
    }

    msg(1, "revoking %s at %s", certfile, url);
    if (200 != acme_post(a, url, "{\"certificate\":\"%s\",\"reason\":%d}",
            crt, reason_code))
    {
        warnx("failed to revoke %s at %s", certfile, url);
        acme_error(a);
        goto out;
    }
    else if (acme_error(a))
    {
        goto out;
    }
    msg(1, "revoked %s", certfile);
    success = true;

out:
    free(crt);
    return success;
}

bool validate_domain_str(const char *s)
{
    bool len = 0;
    for (int j = 0; j < strlen(s); j++)
    {
        switch (s[j])
        {
            case '.':
                if (j == 0)
                {
                    warnx("'.' not allowed at beginning in %s", s);
                    return false;
                }
                // intentional fallthrough
            case '_':
            case '-':
                len++;
                continue;
            case '*':
                if (j != 0 || s[1] != '.')
                {
                    warnx("'*.' only allowed at beginning in %s", s);
                    return false;
                }
                break;
            default:
                if (!isupper(s[j]) && !islower(s[j])
                        && !isdigit(s[j]))
                {
                    warnx("invalid character '%c' in %s", s[j], s);
                    return false;
                }
                len++;
        }
    }
    if (len == 0)
    {
        warnx("empty name is not allowed");
        return false;
    }
    return true;
}

void usage(const char *progname)
{
    fprintf(stderr, "usage: %s [-a|--acme-url URL] [-c|--confdir DIR] [-d|--days DAYS]\n"
            "\t[-f|--force] [-h|--hook PROGRAM] [-n|--never-create] [-s|--staging]\n"
            "\t[-v|--verbose ...] [-V|--version] [-y|--yes] [-?|--help] new [EMAIL]\n"
            "\t| update [EMAIL] | deactivate | issue DOMAIN [ALTNAME ...]]\n"
            "\t| revoke CERTFILE\n", progname);
}

int main(int argc, char **argv)
{
    static struct option options[] =
    {
        {"acme-url",     required_argument, NULL, 'a'},
        {"confdir",      required_argument, NULL, 'c'},
        {"days",         required_argument, NULL, 'd'},
        {"force",        no_argument,       NULL, 'f'},
        {"help",         no_argument,       NULL, '?'},
        {"hook",         required_argument, NULL, 'h'},
        {"never-create", no_argument,       NULL, 'n'},
        {"staging",      no_argument,       NULL, 's'},
        {"verbose",      no_argument,       NULL, 'v'},
        {"version",      no_argument,       NULL, 'V'},
        {"yes",          no_argument,       NULL, 'y'},
        {NULL,           0,                 NULL, 0}
    };

    int ret = EXIT_FAILURE;
    bool never = false;
    bool force = false;
    bool version = false;
    bool yes = false;
    int days = 30;
    const char *revokefile = NULL;

    acme_t a;
    memset(&a, 0, sizeof(a));
    a.directory = PRODUCTION_URL;
    a.confdir = DEFAULT_CONFDIR;

    if (argc < 2)
    {
        usage(basename(argv[0]));
        return ret;
    }

    if (!crypto_init())
    {
        warnx("failed to initialize crypto library");
        return ret;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        warnx("failed to initialize libcurl");
        crypto_deinit();
        return ret;
    }

    while (1)
    {
        char *endptr;
        int option_index;
        int c = getopt_long(argc, argv, "a:c:d:f?h:nsvVy", options, &option_index);
        if (c == -1) break;
        switch (c)
        {
            case 'a':
                a.directory = optarg;
                break;

            case 'c':
                a.confdir = optarg;
                break;

            case 'd':
                days = strtol(optarg, &endptr, 10);
                if (*endptr != 0 || days <= 0)
                {
                    warnx("days must be a positive integer");
                    goto out;
                }
                break;

            case 'f':
                force = true;
                break;

            case 'h':
                a.hook = optarg;
                break;

            case 'n':
                never = true;
                break;

            case 'v':
                g_loglevel++;
                break;

            case 's':
                a.directory = STAGING_URL;
                break;

            case 'V':
                version = true;
                break;

            case 'y':
                yes = true;
                break;

            default:
                usage(basename(argv[0]));
                goto out;
        }
    }

    if (version)
    {
        msg(0, "version " VERSION);
        goto out;
    }

    if (optind == argc)
    {
        usage(basename(argv[0]));
        goto out;
    }

    const char *action = argv[optind++];
    if (strcmp(action, "new") == 0 || strcmp(action, "update") == 0)
    {
        if (optind < argc)
        {
            a.email = argv[optind++];
        }
        if (optind < argc)
        {
            usage(basename(argv[0]));
            goto out;
        }
    }
    else if (strcmp(action, "deactivate") == 0)
    {
        if (optind < argc)
        {
            usage(basename(argv[0]));
            goto out;
        }
    }
    else if (strcmp(action, "issue") == 0)
    {
        if (optind == argc)
        {
            usage(basename(argv[0]));
            goto out;
        }
        a.names = (const char * const *)argv + optind;
        for (const char * const *name = a.names; *name; name++)
        {
            if (!validate_domain_str(*name))
            {
                goto out;
            }
        }

        a.domain = a.names[0];
        if (a.domain[0] == '*' && a.domain[1] == '.')
        {
            a.domain += 2;
        }
    }
    else if (strcmp(action, "revoke") == 0)
    {
        if (optind == argc)
        {
            usage(basename(argv[0]));
            goto out;
        }
        revokefile = argv[optind++];
        if (optind < argc)
        {
            usage(basename(argv[0]));
            goto out;
        }
        if (access(revokefile, R_OK))
        {
            warn("failed to read %s", revokefile);
            goto out;
        }
    }
    else
    {
        usage(basename(argv[0]));
        goto out;
    }

    time_t now = time(NULL);
    char buf[0x100];
    setlocale(LC_TIME, "C");
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
    setlocale(LC_TIME, "");
    msg(1, "version " PACKAGE_VERSION " starting on %s", buf);

    if (a.hook && access(a.hook, R_OK | X_OK) < 0)
    {
        warn("%s", a.hook);
        goto out;
    }

    if (asprintf(&a.keydir, "%s/private", a.confdir) < 0)
    {
        a.keydir = NULL;
        warnx("asprintf failed");
        goto out;
    }

    if (a.domain)
    {
        if (asprintf(&a.dkeydir, "%s/private/%s", a.confdir, a.domain) < 0)
        {
            a.dkeydir = NULL;
            warnx("asprintf failed");
            goto out;
        }

        if (asprintf(&a.certdir, "%s/%s", a.confdir, a.domain) < 0)
        {
            a.certdir = NULL;
            warnx("asprintf failed");
            goto out;
        }
    }

    bool is_new = strcmp(action, "new") == 0;
    if (!check_or_mkdir(is_new && !never, a.confdir,
                S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH))
    {
        goto out;
    }

    if (!check_or_mkdir(is_new && !never, a.keydir, S_IRWXU))
    {
        goto out;
    }

    if (!(a.key = key_load(is_new && !never, "%s/key.pem", a.keydir)))
    {
        goto out;
    }

    if (strcmp(action, "new") == 0)
    {
        if (acme_bootstrap(&a) && account_new(&a, yes))
        {
            ret = EXIT_SUCCESS;
        }
    }
    else if (strcmp(action, "update") == 0)
    {
        if (acme_bootstrap(&a) && account_retrieve(&a) && account_update(&a))
        {
            ret = EXIT_SUCCESS;
        }
    }
    else if (strcmp(action, "deactivate") == 0)
    {
        if (acme_bootstrap(&a) && account_retrieve(&a) && account_deactivate(&a))
        {
            ret = EXIT_SUCCESS;
        }
    }
    else if (strcmp(action, "issue") == 0)
    {
        if (!check_or_mkdir(!never, a.dkeydir, S_IRWXU))
        {
            goto out;
        }

        if (!check_or_mkdir(!never, a.certdir,
                    S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH))
        {
            goto out;
        }

        if (!(a.dkey = key_load(!never, "%s/key.pem", a.dkeydir)))
        {
            goto out;
        }

        msg(1, "checking existence and expiration of %s/cert.pem", a.certdir);
        if (cert_valid(a.certdir, a.names, days))
        {
            if (force)
            {
                msg(1, "forcing reissue of %s/cert.pem", a.certdir);
            }
            else
            {
                msg(1, "skipping %s/cert.pem", a.certdir);
                ret = EXIT_SUCCESS;
                goto out;
            }
        }

        if (acme_bootstrap(&a) && account_retrieve(&a) && cert_issue(&a))
        {
            ret = EXIT_SUCCESS;
        }
    }
    else if (strcmp(action, "revoke") == 0)
    {
        if (acme_bootstrap(&a) && account_retrieve(&a) &&
                cert_revoke(&a, revokefile, 0))
        {
            ret = EXIT_SUCCESS;
        }
    }

out:
    if (a.key) privkey_deinit(a.key);
    if (a.dkey) privkey_deinit(a.dkey);
    json_free(a.json);
    json_free(a.account);
    json_free(a.dir);
    json_free(a.order);
    free(a.nonce);
    free(a.kid);
    free(a.headers);
    free(a.body);
    free(a.type);
    free(a.keydir);
    free(a.dkeydir);
    free(a.certdir);
    curl_global_cleanup();
    crypto_deinit();
    exit(ret);
}

