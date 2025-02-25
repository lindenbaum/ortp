#include "ortp_tester.h"
#include "bctoolbox/logging.h"

static void log_handler(int lev, const char *fmt, va_list args) {
#ifdef _WIN32
	vfprintf(lev == BCTBX_LOG_ERROR ? stderr : stdout, fmt, args);
	fprintf(lev == BCTBX_LOG_ERROR ? stderr : stdout, "\n");
#else
	va_list cap;
	va_copy(cap, args);
	/* Otherwise, we must use stdio to avoid log formatting (for autocompletion etc.) */
	vfprintf(lev == BCTBX_LOG_ERROR ? stderr : stdout, fmt, cap);
	fprintf(lev == BCTBX_LOG_ERROR ? stderr : stdout, "\n");
	va_end(cap);
#endif
}

void ortp_tester_init(void (*ftester_printf)(int level, const char *fmt, va_list args)) {
	if (ftester_printf == NULL)
		ftester_printf = log_handler;
	bc_tester_init(ftester_printf, BCTBX_LOG_MESSAGE, BCTBX_LOG_ERROR, ".");
	bc_tester_add_suite(&fec_test_suite);
}

void ortp_tester_uninit() {
	bc_tester_uninit();
}

int main(int argc, char *argv[]) {
	int i, ret;

	ortp_tester_init(NULL);
	for (i = 1; i < argc; ++i) {
		int ret = bc_tester_parse_args(argc, argv, i);
		if (ret > 0) {
			i += ret - 1;
			continue;
		} else if (ret < 0) {
			bc_tester_helper(argv[0], "");
		}
		return ret;
	}

	ret = bc_tester_start(argv[0]);
	ortp_tester_uninit();
	return ret;
}