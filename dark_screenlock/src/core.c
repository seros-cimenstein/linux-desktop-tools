/*
 * Every key is ignored until the arm combo (Ctrl+Alt+P) is pressed. Then the
 * typed password is buffered blind:
 *   Enter      submit (PAM); wrong password disarms, combo needed again
 *   Alt+C      discard what was typed, stay armed
 *   Backspace  delete last character
 * Pressing the arm combo again while armed also discards the buffer.
 */
#include "core.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef LOCK_PAM_SERVICE
#define LOCK_PAM_SERVICE "login"
#endif

/* X11 keysym values (shared with xkbcommon). */
#define KS_c         0x0063
#define KS_p         0x0070
#define KS_BackSpace 0xff08
#define KS_Return    0xff0d
#define KS_KP_Enter  0xff8d

const char core_keys_help[] =
	"  Ctrl+Alt+P  enable password entry (all other keys are ignored until then)\n"
	"  Enter       submit password; a wrong one disables entry again\n"
	"  Alt+C       discard what you typed and start over\n"
	"  Backspace   delete last character\n";

static const char *user;
static bool armed;
static size_t pw_len;
static char pw[1024];

static void secure_pw_memory(void)
{
	/* Keep the password out of swap. */
	if (mlock(pw, sizeof(pw)) != 0)
		perror("dark-screenlock: mlock");
}

bool core_init(void)
{
	struct passwd *p = getpwuid(getuid());
	if (!p || !(user = strdup(p->pw_name))) {
		fprintf(stderr, "dark-screenlock: cannot determine current user\n");
		return false;
	}
	secure_pw_memory();
	return true;
}

void core_after_fork(void)
{
	secure_pw_memory();
}

void core_wipe(void)
{
	explicit_bzero(pw, sizeof(pw));
	pw_len = 0;
}

static void pw_backspace(void)
{
	/* Drop one UTF-8 code point. */
	while (pw_len > 0) {
		pw_len--;
		unsigned char c = (unsigned char)pw[pw_len];
		pw[pw_len] = '\0';
		if ((c & 0xC0) != 0x80)
			break;
	}
}

static int pam_conv_fn(int n, const struct pam_message **msg,
		       struct pam_response **resp, void *data)
{
	(void)data;
	struct pam_response *r = calloc((size_t)n, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;

	for (int i = 0; i < n; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(pw);
			if (!r[i].resp)
				goto fail;
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			break;
		default:
			goto fail;
		}
	}
	*resp = r;
	return PAM_SUCCESS;

fail:
	for (int i = 0; i < n; i++) {
		if (r[i].resp) {
			explicit_bzero(r[i].resp, strlen(r[i].resp));
			free(r[i].resp);
		}
	}
	free(r);
	return PAM_CONV_ERR;
}

static bool authenticate(void)
{
	struct pam_conv conv = { .conv = pam_conv_fn };
	pam_handle_t *h;
	int ret = pam_start(LOCK_PAM_SERVICE, user, &conv, &h);
	if (ret != PAM_SUCCESS) {
		fprintf(stderr, "dark-screenlock: pam_start failed: %s\n",
			pam_strerror(NULL, ret));
		return false;
	}
	ret = pam_authenticate(h, 0);
	if (ret == PAM_SUCCESS)
		pam_setcred(h, PAM_REFRESH_CRED);
	pam_end(h, ret);
	return ret == PAM_SUCCESS;
}

static bool submit(void)
{
	if (pw_len == 0)
		return false;
	bool ok = authenticate();
	core_wipe();
	armed = false;
	return ok;
}

bool core_key(uint32_t base, uint32_t sym, bool ctrl, bool alt,
	      const char *text, size_t len)
{
	if (ctrl && alt && base == KS_p) {
		core_wipe();
		armed = true;
		return false;
	}
	if (!armed)
		return false;
	if (alt && !ctrl && base == KS_c) {
		core_wipe();
		return false;
	}

	switch (sym) {
	case KS_Return:
	case KS_KP_Enter:
		return submit();
	case KS_BackSpace:
		pw_backspace();
		return false;
	}

	if (ctrl || alt || len == 0)
		return false;
	if ((unsigned char)text[0] < 0x20 || text[0] == 0x7f)
		return false;
	if (pw_len + len < sizeof(pw)) {
		memcpy(pw + pw_len, text, len);
		pw_len += len;
	}
	return false;
}
