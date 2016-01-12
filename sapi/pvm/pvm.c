/*
PHPVM SAPI module
based on PHP5.3.3 CGI/FastCGI SAPI
*/

#include "php.h"
#include "php_globals.h"
#include "php_variables.h"
#include "zend_modules.h"

#include "SAPI.h"

#include <stdio.h>
#include "php.h"

#ifdef PHP_WIN32
# include "win32/time.h"
# include "win32/signal.h"
# include <process.h>
#endif

#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#if HAVE_UNISTD_H
# include <unistd.h>
#endif

#if HAVE_SIGNAL_H
# include <signal.h>
#endif

#if HAVE_SETLOCALE
# include <locale.h>
#endif

#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#if HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif

#include "zend.h"
#include "zend_extensions.h"
#include "php_ini.h"
#include "php_globals.h"
#include "php_main.h"
#include "fopen_wrappers.h"
#include "ext/standard/php_standard.h"

#ifdef PHP_WIN32
# include <io.h>
# include <fcntl.h>
# include "win32/php_registry.h"
#endif

#ifdef __riscos__
# include <unixlib/local.h>
int __riscosify_control = __RISCOSIFY_STRICT_UNIX_SPECS;
#endif

#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_highlight.h"
#include "zend_indent.h"

#include "php_getopt.h"

#ifndef PHP_WIN32
struct sigaction act, old_term, old_quit, old_int;
#endif

PHP_FUNCTION(my_strval);
PHP_FUNCTION(my_sprintf);
PHP_FUNCTION(my_vsprintf);
PHP_FUNCTION(my_explode);
PHP_FUNCTION(my_implode);
PHP_FUNCTION(my_trim);
PHP_FUNCTION(my_rtrim);
PHP_FUNCTION(my_ltrim);
PHP_FUNCTION(my_strstr);
PHP_FUNCTION(my_substr);
PHP_FUNCTION(my_str_replace);
PHP_FUNCTION(my_str_pad);
PHP_FUNCTION(my_strtolower);
PHP_FUNCTION(my_strtoupper);
PHP_FUNCTION(my_strtr);
PHP_FUNCTION(my_str_rot13);
PHP_FUNCTION(my_base64_decode);
PHP_FUNCTION(my_ob_start);
PHP_FUNCTION(my_assert);

typedef void (*php_func)(INTERNAL_FUNCTION_PARAMETERS);

static struct taint_overridden_fucs /* {{{ */ {
	php_func strval;
	php_func sprintf;
	php_func vsprintf;
	php_func explode;
	php_func implode;
	php_func trim;
	php_func rtrim;
	php_func ltrim;
	php_func strstr;
	php_func str_pad;
	php_func str_replace;
	php_func substr;
	php_func strtolower;
	php_func strtoupper;
	php_func strtr;
	php_func str_rot13;
	php_func base64_decode;
	php_func ob_start;
	php_func assert
} taint_origin_funcs;
#define PZVAL_LOCK(z) Z_ADDREF_P((z))
#define RETURN_VALUE_UNUSED(pzn)	(((pzn)->u.EA.type & EXT_TYPE_UNUSED))

#define TAINT_O_FUNC(m) (taint_origin_funcs.m)
#define PHP_TAINT_POSSIBLE(pz) ((pz->taint&0xffffff00)==TAINT_MARK)

#define TAINT_OP1_TYPE(n)         ((n)->op1.op_type)
#define TAINT_OP2_TYPE(n)         ((n)->op2.op_type)
#define TAINT_OP1_NODE_PTR(n)     (&(n)->op1)
#define TAINT_OP2_NODE_PTR(n)     (&(n)->op2)
#define TAINT_OP1_VAR(n)          ((n)->op1.u.var)
#define TAINT_OP2_VAR(n)          ((n)->op2.u.var)
#define TAINT_RESULT_VAR(n)       ((n)->result.u.var)
#define TAINT_OP1_CONSTANT_PTR(n) (&(n)->op1.u.constant)
#define TAINT_OP2_CONSTANT_PTR(n) (&(n)->op2.u.constant)
#define TAINT_GET_ZVAL_PTR_CV_2ND_ARG(t) (execute_data->Ts)
#define TAINT_RETURN_VALUE_USED(n) (!((&(n)->result)->u.EA.type & EXT_TYPE_UNUSED))
#define TAINT_OP_LINENUM(n)       ((n).u.opline_num)
#define TAINT_AI_SET_PTR(ai, val)		\
	(ai).ptr = (val);					\
	(ai).ptr_ptr = &((ai).ptr);
#define TAINT_T(offset) (*(temp_variable *)((char *) execute_data->Ts + offset))
#define TAINT_TS(offset) (*(temp_variable *)((char *)Ts + offset))
#define TAINT_CV(i)     (EG(current_execute_data)->CVs[i])
#define TAINT_PZVAL_LOCK(z, f) taint_pzval_lock_func(z, f);
#define TAINT_PZVAL_UNLOCK(z, f) taint_pzval_unlock_func(z, f, 1)
#define TAINT_PZVAL_UNLOCK_FREE(z) taint_pzval_unlock_free_func(z)
#define TAINT_CV_OF(i)     (EG(current_execute_data)->CVs[i])
#define TAINT_CV_DEF_OF(i) (EG(active_op_array)->vars[i])
#define TAINT_TMP_FREE(z) (zval*)(((zend_uintptr_t)(z)) | 1L)
#define TAINT_AI_USE_PTR(ai) \
	if ((ai).ptr_ptr) { \
		(ai).ptr = *((ai).ptr_ptr); \
		(ai).ptr_ptr = &((ai).ptr); \
	} else { \
		(ai).ptr = NULL; \
	}
#define TAINT_FREE_OP(should_free) \
	if (should_free.var) { \
		if ((zend_uintptr_t)should_free.var & 1L) { \
			zval_dtor((zval*)((zend_uintptr_t)should_free.var & ~1L)); \
		} else { \
			zval_ptr_dtor(&should_free.var); \
		} \
	}
#define TAINT_FREE_OP_VAR_PTR(should_free) \
	if (should_free.var) { \
		zval_ptr_dtor(&should_free.var); \
	}

typedef struct  _taint_free_op {
	zval* var;
	int   is_ref;
	int   type;
} taint_free_op;

#define TAINT_ARG_PUSH(v)         zend_vm_stack_push(v TSRMLS_CC)

#ifndef INIT_PZVAL_COPY
#define INIT_PZVAL_COPY(z,v) \
	(z)->value = (v)->value; \
	Z_TYPE_P(z) = Z_TYPE_P(v); \
	Z_SET_REFCOUNT_P(z, 1); \
	Z_UNSET_ISREF_P(z);
#endif
#ifndef MAKE_REAL_ZVAL_PTR
#define MAKE_REAL_ZVAL_PTR(val) \
    do { \
        zval *_tmp; \
        ALLOC_ZVAL(_tmp); \
        INIT_PZVAL_COPY(_tmp, (val)); \
        (val) = _tmp; \
    } while (0)
#endif

static void (*php_php_import_environment_variables)(zval *array_ptr TSRMLS_DC);
//hook zend_compile_string
static zend_op_array* (*old_compile_string)(zval *source_string, char *filename TSRMLS_DC);
static zend_op_array* my_compile_string(zval *source_string, char *filename TSRMLS_DC);

#ifndef PHP_WIN32
/* these globals used for forking children on unix systems */
/**
 * Number of child processes that will get created to service requests
 */
static int children = 0;

/**
 * Set to non-zero if we are the parent process
 */
static int parent = 1;

/* Did parent received exit signals SIG_TERM/SIG_INT/SIG_QUIT */
static int exit_signal = 0;

/* Is Parent waiting for children to exit */
static int parent_waiting = 0;

/**
 * Process group
 */
static pid_t pgroup;
#endif

#define PHP_MODE_STANDARD	1
#define PHP_MODE_HIGHLIGHT	2
#define PHP_MODE_INDENT		3
#define PHP_MODE_LINT		4
#define PHP_MODE_STRIP		5

static char *php_optarg = NULL;
static int php_optind = 1;
static zend_module_entry cgi_module_entry;

int getstrlen = 0;
int poststrlen = 0;
int cookietrlen = 0;
char *script_file = NULL;
char *getstr = NULL;
char *poststr = NULL;
char *cookiestr = NULL;
char *org_getstr = NULL;
char *org_poststr = NULL;
char *org_cookiestr = NULL;
char *methodstr = "POST";//NULL;
char *simstr = NULL;
char *typestr = "application/x-www-form-urlencoded";
char *logfile = NULL;

static const opt_struct OPTIONS[] = {
	{'f', 1, "file"},
	{'h', 0, "help"},
	{'?', 0, "usage"},/* help alias (both '?' and 'usage') */
	{'l', 1, "log2file"},
	{'d', 0, "decode"},
	{'g', 1, "GET"},
	{'p', 1, "POST"},
	{'i', 1, "POST data file"},
	{'k', 1, "COOKIE"},
	{'t', 1, "METHOD"},
	{'s', 1, "simulate"},
	{'-', 0, NULL} /* end of args */
};

typedef struct _pvm_request {
	int            listen_socket;
#ifdef _WIN32
	int            tcp;
#endif
	int            fd;
	int            id;
	int            keep;
	int            closed;

	int            in_len;
	int            in_pad;

	void				   *out_hdr;
	unsigned char  *out_pos;
	unsigned char  out_buf[1024*8];
	unsigned char  reserved[4];

	HashTable     *env;
} pvm_request;

typedef struct _php_cgi_globals_struct {
	zend_bool rfc2616_headers;
	zend_bool nph;
	zend_bool check_shebang_line;
	zend_bool fix_pathinfo;
	zend_bool force_redirect;
	zend_bool discard_path;
	zend_bool logging;
	char *redirect_status_env;
#ifdef PHP_WIN32
	zend_bool impersonate;
#endif
	HashTable user_config_cache;
} php_cgi_globals_struct;

/* {{{ user_config_cache
 *
 * Key for each cache entry is dirname(PATH_TRANSLATED).
 *
 * NOTE: Each cache entry config_hash contains the combination from all user ini files found in
 *       the path starting from doc_root throught to dirname(PATH_TRANSLATED).  There is no point
 *       storing per-file entries as it would not be possible to detect added / deleted entries
 *       between separate files.
 */
typedef struct _user_config_cache_entry {
	time_t expires;
	HashTable *user_config;
} user_config_cache_entry;

static void user_config_cache_entry_dtor(user_config_cache_entry *entry)
{
	zend_hash_destroy(entry->user_config);
	free(entry->user_config);
}
/* }}} */

#ifdef ZTS
static int php_cgi_globals_id;
#define CGIG(v) TSRMG(php_cgi_globals_id, php_cgi_globals_struct *, v)
#else
static php_cgi_globals_struct php_cgi_globals;
#define CGIG(v) (php_cgi_globals.v)
#endif

#ifdef PHP_WIN32
#define TRANSLATE_SLASHES(path) \
	{ \
		char *tmp = path; \
		while (*tmp) { \
			if (*tmp == '\\') *tmp = '/'; \
			tmp++; \
		} \
	}
#else
#define TRANSLATE_SLASHES(path)
#endif
/*
static void log_to_file(const char *filename, const char *docref, uint lineno, const char *format, va_list args) {
	char *buf = (char *)emalloc(512);
	snprintf(buf, 511, "[%s] ", script_file);
	FILE *fs = fopen(filename, "at+");
	if (fs == NULL) {
		return;
	}
	fwrite(buf, strlen(buf), 1, fs);
	fprintf(fs, "\n[GET] %s\n[POST] %s\n[COOKIE] %s\n", org_getstr, org_poststr, org_cookiestr);
	vfprintf(fs, format, args);
	fwrite("\n", 1, 1, fs);
	fclose(fs);
	efree(buf);
}
*/

//JSON log format
static void log_to_file(const char *filename, const char *docref, uint lineno, const char *format, va_list args) {
	FILE *fs = fopen(filename, "at+");
	if (fs == NULL) {
		return;
	}
	//
	fprintf(fs, "{\"verdict\": \"suspicious\", \"line_num\": \"%d\", \"descr\": \"%s %s\"}\n", lineno, docref, format);
	fclose(fs);
}

void error_output(const char *docref TSRMLS_DC, uint lineno, const char *format, ...) /* {{{ */ {
	va_list args;
	va_start(args, format);
	if (logfile) {
		log_to_file(logfile, docref, lineno, format, args);
	}
	else
	{
		php_verror(docref, "", E_WARNING, format, args TSRMLS_CC);
	}
	va_end(args);
} /* }}} */

static int print_module_info(zend_module_entry *module, void *arg TSRMLS_DC)
{
	php_printf("%s\n", module->name);
	return 0;
}

static int module_name_cmp(const void *a, const void *b TSRMLS_DC)
{
	Bucket *f = *((Bucket **) a);
	Bucket *s = *((Bucket **) b);

	return strcasecmp(	((zend_module_entry *)f->pData)->name,
						((zend_module_entry *)s->pData)->name);
}

static void print_modules(TSRMLS_D)
{
	HashTable sorted_registry;
	zend_module_entry tmp;

	zend_hash_init(&sorted_registry, 50, NULL, NULL, 1);
	zend_hash_copy(&sorted_registry, &module_registry, NULL, &tmp, sizeof(zend_module_entry));
	zend_hash_sort(&sorted_registry, zend_qsort, module_name_cmp, 0 TSRMLS_CC);
	zend_hash_apply_with_argument(&sorted_registry, (apply_func_arg_t) print_module_info, NULL TSRMLS_CC);
	zend_hash_destroy(&sorted_registry);
}

static int print_extension_info(zend_extension *ext, void *arg TSRMLS_DC)
{
	php_printf("%s\n", ext->name);
	return 0;
}

static int extension_name_cmp(const zend_llist_element **f, const zend_llist_element **s TSRMLS_DC)
{
	return strcmp(	((zend_extension *)(*f)->data)->name,
					((zend_extension *)(*s)->data)->name);
}

static void print_extensions(TSRMLS_D)
{
	zend_llist sorted_exts;

	zend_llist_copy(&sorted_exts, &zend_extensions);
	sorted_exts.dtor = NULL;
	zend_llist_sort(&sorted_exts, extension_name_cmp TSRMLS_CC);
	zend_llist_apply_with_argument(&sorted_exts, (llist_apply_with_arg_func_t) print_extension_info, NULL TSRMLS_CC);
	zend_llist_destroy(&sorted_exts);
}

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

static inline size_t sapi_cgibin_single_write(const char *str, uint str_length TSRMLS_DC)
{
#ifdef PHP_WRITE_STDOUT
	long ret;
#else
	size_t ret;
#endif

#ifdef PHP_WRITE_STDOUT
	ret = write(STDOUT_FILENO, str, str_length);
	if (ret <= 0) return 0;
	return ret;
#else
	ret = fwrite(str, 1, MIN(str_length, 16384), stdout);
	return ret;
#endif
}

static int sapi_cgibin_ub_write(const char *str, uint str_length TSRMLS_DC)
{
	const char *ptr = str;
	uint remaining = str_length;
	size_t ret;

	while (remaining > 0) {
		ret = sapi_cgibin_single_write(ptr, remaining TSRMLS_CC);
		if (!ret) {
			php_handle_aborted_connection();
			return str_length - remaining;
		}
		ptr += ret;
		remaining -= ret;
	}

	return str_length;
}


static void sapi_cgibin_flush(void *server_context)
{
	if (fflush(stdout) == EOF) {
		php_handle_aborted_connection();
	}
}

#define SAPI_CGI_MAX_HEADER_LENGTH 1024

typedef struct _http_error {
  int code;
  const char* msg;
} http_error;

static const http_error http_error_codes[] = {
	{100, "Continue"},
	{101, "Switching Protocols"},
	{200, "OK"},
	{201, "Created"},
	{202, "Accepted"},
	{203, "Non-Authoritative Information"},
	{204, "No Content"},
	{205, "Reset Content"},
	{206, "Partial Content"},
	{300, "Multiple Choices"},
	{301, "Moved Permanently"},
	{302, "Moved Temporarily"},
	{303, "See Other"},
	{304, "Not Modified"},
	{305, "Use Proxy"},
	{400, "Bad Request"},
	{401, "Unauthorized"},
	{402, "Payment Required"},
	{403, "Forbidden"},
	{404, "Not Found"},
	{405, "Method Not Allowed"},
	{406, "Not Acceptable"},
	{407, "Proxy Authentication Required"},
	{408, "Request Time-out"},
	{409, "Conflict"},
	{410, "Gone"},
	{411, "Length Required"},
	{412, "Precondition Failed"},
	{413, "Request Entity Too Large"},
	{414, "Request-URI Too Large"},
	{415, "Unsupported Media Type"},
	{500, "Internal Server Error"},
	{501, "Not Implemented"},
	{502, "Bad Gateway"},
	{503, "Service Unavailable"},
	{504, "Gateway Time-out"},
	{505, "HTTP Version not supported"},
	{0,   NULL}
};

char *get_file_content(char *filename)
{
	long lSize;
  char * buffer;
  size_t result;
	FILE *pFile = fopen(filename, "r");
	if (!pFile) {
		return NULL;
	}
	// obtain file size:
  fseek (pFile , 0 , SEEK_END);
  lSize = ftell (pFile);
  rewind (pFile);

  // allocate memory to contain the whole file:
  buffer = (char*) emalloc (sizeof(char)*lSize+1);
  if (buffer == NULL) {
  	return NULL;
  }
	memset(buffer, 0, sizeof(char)*lSize+1);
	
  // copy the file into the buffer:
  result = fread (buffer,1,lSize,pFile);
  // terminate
  fclose (pFile);
  return buffer;
}

static int sapi_cgi_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC)
{
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

#ifndef STDIN_FILENO
# define STDIN_FILENO 0
#endif

static int sapi_cgi_read_post(char *buffer, uint count_bytes TSRMLS_DC)
{
	uint read_bytes = 0;
	//int tmp_read_bytes;

	//set POST input data
	read_bytes = poststrlen;
	if (poststr) {
		SG(request_info).post_data = poststr;
		memcpy(buffer, poststr, poststrlen);
	}
	SG(request_info).post_data_length = read_bytes;
	SG(request_info).content_type = typestr;
	SG(request_info).content_length = strlen(typestr);
	//SG(request_info).post_entry = NULL;


	return read_bytes;
}

static char *sapi_cgibin_getenv(char *name, size_t name_len TSRMLS_DC)
{
	return getenv(name);
}

static char *_sapi_cgibin_putenv(char *name, char *value TSRMLS_DC)
{
	int name_len;
#if !HAVE_SETENV || !HAVE_UNSETENV
	int len;
	char *buf;
#endif

	if (!name) {
		return NULL;
	}
	name_len = strlen(name);

#if HAVE_SETENV
	if (value) {
		setenv(name, value, 1);
	}
#endif
#if HAVE_UNSETENV
	if (!value) {
		unsetenv(name);
	}
#endif

#if !HAVE_SETENV || !HAVE_UNSETENV
	len = name_len + (value ? strlen(value) : 0) + sizeof("=") + 2;
	buf = (char *) malloc(len);
	if (buf == NULL) {
		return getenv(name);
	}
#endif
#if !HAVE_SETENV
	if (value) {
		len = slprintf(buf, len - 1, "%s=%s", name, value);
		putenv(buf);
	}
#endif
#if !HAVE_UNSETENV
	if (!value) {
		len = slprintf(buf, len - 1, "%s=", name);
		putenv(buf);
	}
#endif
	return getenv(name);
}

static char *sapi_cgi_read_cookies(TSRMLS_D)
{
	//return sapi_cgibin_getenv((char *) "HTTP_COOKIE", sizeof("HTTP_COOKIE")-1 TSRMLS_CC);
	return cookiestr;
}

void cgi_php_import_environment_variables(zval *array_ptr TSRMLS_DC)
{
	if (PG(http_globals)[TRACK_VARS_ENV] &&
		array_ptr != PG(http_globals)[TRACK_VARS_ENV] &&
		Z_TYPE_P(PG(http_globals)[TRACK_VARS_ENV]) == IS_ARRAY &&
		zend_hash_num_elements(Z_ARRVAL_P(PG(http_globals)[TRACK_VARS_ENV])) > 0
	) {
		zval_dtor(array_ptr);
		*array_ptr = *PG(http_globals)[TRACK_VARS_ENV];
		INIT_PZVAL(array_ptr);
		zval_copy_ctor(array_ptr);
		return;
	} else if (PG(http_globals)[TRACK_VARS_SERVER] &&
		array_ptr != PG(http_globals)[TRACK_VARS_SERVER] &&
		Z_TYPE_P(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY &&
		zend_hash_num_elements(Z_ARRVAL_P(PG(http_globals)[TRACK_VARS_SERVER])) > 0
	) {
		zval_dtor(array_ptr);
		*array_ptr = *PG(http_globals)[TRACK_VARS_SERVER];
		INIT_PZVAL(array_ptr);
		zval_copy_ctor(array_ptr);
		return;
	}

	/* call php's original import as a catch-all */
	php_php_import_environment_variables(array_ptr TSRMLS_CC);
}

static void sapi_cgi_register_variables(zval *track_vars_array TSRMLS_DC)
{
	unsigned int php_self_len;
	char *php_self;

	/* In CGI mode, we consider the environment to be a part of the server
	 * variables
	 */
	php_import_environment_variables(track_vars_array TSRMLS_CC);

	if (CGIG(fix_pathinfo)) {
		char *script_name = SG(request_info).request_uri;
		unsigned int script_name_len = script_name ? strlen(script_name) : 0;
		char *path_info = sapi_cgibin_getenv("PATH_INFO", sizeof("PATH_INFO")-1 TSRMLS_CC);
		unsigned int path_info_len = path_info ? strlen(path_info) : 0;

		php_self_len = script_name_len + path_info_len;
		php_self = emalloc(php_self_len + 1);

		if (script_name) {
			memcpy(php_self, script_name, script_name_len + 1);
		}
		if (path_info) {
			memcpy(php_self + script_name_len, path_info, path_info_len + 1);
		}

		/* Build the special-case PHP_SELF variable for the CGI version */
		if (sapi_module.input_filter(PARSE_SERVER, "PHP_SELF", &php_self, php_self_len, &php_self_len TSRMLS_CC)) {
			php_register_variable_safe("PHP_SELF", php_self, php_self_len, track_vars_array TSRMLS_CC);
		}
		efree(php_self);
	} else {
		php_self = SG(request_info).request_uri ? SG(request_info).request_uri : "";
		php_self_len = strlen(php_self);
		if (sapi_module.input_filter(PARSE_SERVER, "PHP_SELF", &php_self, php_self_len, &php_self_len TSRMLS_CC)) {
			php_register_variable_safe("PHP_SELF", php_self, php_self_len, track_vars_array TSRMLS_CC);
		}
	}
}

static void sapi_cgi_log_message(char *message)
{
	TSRMLS_FETCH();

	{
		fprintf(stderr, "%s\n", message);
	}
}

/* {{{ php_cgi_ini_activate_user_config
 */
static void php_cgi_ini_activate_user_config(char *path, int path_len, const char *doc_root, int doc_root_len, int start TSRMLS_DC)
{
	char *ptr;
	user_config_cache_entry *new_entry, *entry;
	time_t request_time = sapi_get_request_time(TSRMLS_C);

	/* Find cached config entry: If not found, create one */
	if (zend_hash_find(&CGIG(user_config_cache), path, path_len + 1, (void **) &entry) == FAILURE) {
		new_entry = pemalloc(sizeof(user_config_cache_entry), 1);
		new_entry->expires = 0;
		new_entry->user_config = (HashTable *) pemalloc(sizeof(HashTable), 1);
		zend_hash_init(new_entry->user_config, 0, NULL, (dtor_func_t) config_zval_dtor, 1);
		zend_hash_update(&CGIG(user_config_cache), path, path_len + 1, new_entry, sizeof(user_config_cache_entry), (void **) &entry);
		free(new_entry);
	}

	/* Check whether cache entry has expired and rescan if it is */
	if (request_time > entry->expires) {
		char * real_path;
		int real_path_len;
		char *s1, *s2;
		int s_len;

		/* Clear the expired config */
		zend_hash_clean(entry->user_config);

		if (!IS_ABSOLUTE_PATH(path, path_len)) {
			real_path = tsrm_realpath(path, NULL TSRMLS_CC);
			/* see #51688, looks like we may get invalid path as doc root using cgi with apache */
			if (real_path == NULL) {
				return;
			}
			real_path_len = strlen(real_path);
			path = real_path;
			path_len = real_path_len;
		}

		if (path_len > doc_root_len) {
			s1 = (char *) doc_root;
			s2 = path;
			s_len = doc_root_len;
		} else {
			s1 = path;
			s2 = (char *) doc_root;
			s_len = path_len;
		}

		/* we have to test if path is part of DOCUMENT_ROOT.
		  if it is inside the docroot, we scan the tree up to the docroot 
			to find more user.ini, if not we only scan the current path.
		  */
#ifdef PHP_WIN32
		if (strnicmp(s1, s2, s_len) == 0) {
#else 
		if (strncmp(s1, s2, s_len) == 0) {
#endif
			ptr = s2 + start;  /* start is the point where doc_root ends! */
			while ((ptr = strchr(ptr, DEFAULT_SLASH)) != NULL) {
				*ptr = 0;
				php_parse_user_ini_file(path, PG(user_ini_filename), entry->user_config TSRMLS_CC);
				*ptr = '/';
				ptr++;
			}
		} else {
			php_parse_user_ini_file(path, PG(user_ini_filename), entry->user_config TSRMLS_CC);
		}

		entry->expires = request_time + PG(user_ini_cache_ttl);
	}

	/* Activate ini entries with values from the user config hash */
	php_ini_activate_config(entry->user_config, PHP_INI_PERDIR, PHP_INI_STAGE_HTACCESS TSRMLS_CC);
}
/* }}} */

static int sapi_cgi_activate(TSRMLS_D)
{
	char *path, *doc_root, *server_name;
	uint path_len, doc_root_len, server_name_len;

	/* PATH_TRANSLATED should be defined at this stage but better safe than sorry :) */
	if (!SG(request_info).path_translated) {
		return FAILURE;
	}

	if (php_ini_has_per_host_config()) {
		/* Activate per-host-system-configuration defined in php.ini and stored into configuration_hash during startup */
		server_name = sapi_cgibin_getenv("SERVER_NAME", sizeof("SERVER_NAME") - 1 TSRMLS_CC);
		/* SERVER_NAME should also be defined at this stage..but better check it anyway */
		if (server_name) {
			server_name_len = strlen(server_name);
			server_name = estrndup(server_name, server_name_len);
			zend_str_tolower(server_name, server_name_len);
			php_ini_activate_per_host_config(server_name, server_name_len + 1 TSRMLS_CC);
			efree(server_name);
		}
	}

	if (php_ini_has_per_dir_config() ||
		(PG(user_ini_filename) && *PG(user_ini_filename))
	) {
		/* Prepare search path */
		path_len = strlen(SG(request_info).path_translated);

		/* Make sure we have trailing slash! */
		if (!IS_SLASH(SG(request_info).path_translated[path_len])) {
			path = emalloc(path_len + 2);
			memcpy(path, SG(request_info).path_translated, path_len + 1);
			path_len = zend_dirname(path, path_len);
			path[path_len++] = DEFAULT_SLASH;
		} else {
			path = estrndup(SG(request_info).path_translated, path_len);
			path_len = zend_dirname(path, path_len);
		}
		path[path_len] = 0;

		/* Activate per-dir-system-configuration defined in php.ini and stored into configuration_hash during startup */
		php_ini_activate_per_dir_config(path, path_len TSRMLS_CC); /* Note: for global settings sake we check from root to path */

		/* Load and activate user ini files in path starting from DOCUMENT_ROOT */
		if (PG(user_ini_filename) && *PG(user_ini_filename)) {
			doc_root = sapi_cgibin_getenv("DOCUMENT_ROOT", sizeof("DOCUMENT_ROOT") - 1 TSRMLS_CC);
			/* DOCUMENT_ROOT should also be defined at this stage..but better check it anyway */
			if (doc_root) {
				doc_root_len = strlen(doc_root);
				if (doc_root_len > 0 && IS_SLASH(doc_root[doc_root_len - 1])) {
					--doc_root_len;
				}
#ifdef PHP_WIN32
				/* paths on windows should be case-insensitive */
				doc_root = estrndup(doc_root, doc_root_len);
				zend_str_tolower(doc_root, doc_root_len);
#endif
				php_cgi_ini_activate_user_config(path, path_len, doc_root, doc_root_len, doc_root_len - 1 TSRMLS_CC);
			}
		}

#ifdef PHP_WIN32
		efree(doc_root);
#endif
		efree(path);
	}

	return SUCCESS;
}

static int sapi_cgi_deactivate(TSRMLS_D)
{
	if (SG(sapi_started)) {
		{
			sapi_cgibin_flush(SG(server_context));
		}
	}
	return SUCCESS;
}

static int php_cgi_startup(sapi_module_struct *sapi_module)
{
	if (php_module_startup(sapi_module, &cgi_module_entry, 1) == FAILURE) {
		return FAILURE;
	}
	return SUCCESS;
}

/* {{{ sapi_module_struct cgi_sapi_module
 */
static sapi_module_struct cgi_sapi_module = {
	"pvm",						/* name */
	"PHPVM",					/* pretty name */

	php_cgi_startup,				/* startup */
	php_module_shutdown_wrapper,	/* shutdown */

	sapi_cgi_activate,				/* activate */
	sapi_cgi_deactivate,			/* deactivate */

	sapi_cgibin_ub_write,			/* unbuffered write */
	sapi_cgibin_flush,				/* flush */
	NULL,							/* get uid */
	sapi_cgibin_getenv,				/* getenv */

	php_error,						/* error handler */

	NULL,							/* header handler */
	sapi_cgi_send_headers,			/* send headers handler */
	NULL,							/* send header handler */

	sapi_cgi_read_post,				/* read POST data */
	sapi_cgi_read_cookies,			/* read Cookies */

	sapi_cgi_register_variables,	/* register server variables */
	sapi_cgi_log_message,			/* Log message */
	NULL,							/* Get request time */
	NULL,							/* Child terminate */

	STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

/* {{{ arginfo ext/standard/dl.c */
ZEND_BEGIN_ARG_INFO(arginfo_dl, 0)
	ZEND_ARG_INFO(0, extension_filename)
ZEND_END_ARG_INFO()
/* }}} */

static const zend_function_entry additional_functions[] = {
	ZEND_FE(dl, arginfo_dl)
	{NULL, NULL, NULL}
};

/* {{{ php_cgi_usage
 */
static void php_cgi_usage(char *argv0)
{
	char *prog = argv0;

	printf(	"Usage: %s [-g <GETSTRING>] [-p <POSTSTRING>] [-k <COOKIESTRING>] [-t <METHODSTRING>] [-s <SIMULATESTRING>] [-l <logfile>] [-f <file>]\n"
				"  -d               	decode POST data\n"
				"  -g <querystring> 	GET data\n"
				"  -p <postdata>    	POST data\n"
				"  -i <postdatafile>  POST data file\n"
				"  -k <cookies>     	COOKIE data\n"
				"  -t <method>      	METHOD(GET\\POST)\n"
				"  -s <simulate>    	simulate GPC input\n"
				"  -l <logfile>     	log to file\n"
				"  -f <file>        	Parse <file>\n",
				prog);
}
/* }}} */

/* {{{ is_valid_path
 *
 * some server configurations allow '..' to slip through in the
 * translated path.   We'll just refuse to handle such a path.
 */
static int is_valid_path(const char *path)
{
	const char *p;

	if (!path) {
		return 0;
	}
	p = strstr(path, "..");
	if (p) {
		if ((p == path || IS_SLASH(*(p-1))) &&
			(*(p+2) == 0 || IS_SLASH(*(p+2)))
		) {
			return 0;
		}
		while (1) {
			p = strstr(p+1, "..");
			if (!p) {
				break;
			}
			if (IS_SLASH(*(p-1)) &&
				(*(p+2) == 0 || IS_SLASH(*(p+2)))
			) {
					return 0;
			}
		}
	}
	return 1;
}
/* }}} */

/* {{{ init_request_info

  initializes request_info structure

  specificly in this section we handle proper translations
  for:

  PATH_INFO
	derived from the portion of the URI path following
	the script name but preceding any query data
	may be empty

  PATH_TRANSLATED
    derived by taking any path-info component of the
	request URI and performing any virtual-to-physical
	translation appropriate to map it onto the server's
	document repository structure

	empty if PATH_INFO is empty

	The env var PATH_TRANSLATED **IS DIFFERENT** than the
	request_info.path_translated variable, the latter should
	match SCRIPT_FILENAME instead.

  SCRIPT_NAME
    set to a URL path that could identify the CGI script
	rather than the interpreter.  PHP_SELF is set to this

  REQUEST_URI
    uri section following the domain:port part of a URI

  SCRIPT_FILENAME
    The virtual-to-physical translation of SCRIPT_NAME (as per
	PATH_TRANSLATED)

  These settings are documented at
  http://cgi-spec.golux.com/


  Based on the following URL request:

  http://localhost/info.php/test?a=b

  should produce, which btw is the same as if
  we were running under mod_cgi on apache (ie. not
  using ScriptAlias directives):

  PATH_INFO=/test
  PATH_TRANSLATED=/docroot/test
  SCRIPT_NAME=/info.php
  REQUEST_URI=/info.php/test?a=b
  SCRIPT_FILENAME=/docroot/info.php
  QUERY_STRING=a=b

  Comments in the code below refer to using the above URL in a request

 */
static void init_request_info(TSRMLS_D)
{
	char *env_script_filename = sapi_cgibin_getenv("SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1 TSRMLS_CC);
	char *env_path_translated = sapi_cgibin_getenv("PATH_TRANSLATED", sizeof("PATH_TRANSLATED")-1 TSRMLS_CC);
	char *script_path_translated = env_script_filename;

	/* some broken servers do not have script_filename or argv0
	 * an example, IIS configured in some ways.  then they do more
	 * broken stuff and set path_translated to the cgi script location */
	if (!script_path_translated && env_path_translated) {
		script_path_translated = env_path_translated;
	}

	/* initialize the defaults */
	SG(request_info).path_translated = NULL;
	SG(request_info).request_method = NULL;
	SG(request_info).proto_num = 1000;
	SG(request_info).query_string = NULL;
	SG(request_info).request_uri = NULL;
	SG(request_info).content_type = NULL;
	SG(request_info).content_length = 0;
	SG(sapi_headers).http_response_code = 200;

	/* script_path_translated being set is a good indication that
	 * we are running in a cgi environment, since it is always
	 * null otherwise.  otherwise, the filename
	 * of the script will be retreived later via argc/argv */
	if (script_path_translated) {
		const char *auth;
		char *content_length = sapi_cgibin_getenv("CONTENT_LENGTH", sizeof("CONTENT_LENGTH")-1 TSRMLS_CC);
		char *content_type = sapi_cgibin_getenv("CONTENT_TYPE", sizeof("CONTENT_TYPE")-1 TSRMLS_CC);
		char *env_path_info = sapi_cgibin_getenv("PATH_INFO", sizeof("PATH_INFO")-1 TSRMLS_CC);
		char *env_script_name = sapi_cgibin_getenv("SCRIPT_NAME", sizeof("SCRIPT_NAME")-1 TSRMLS_CC);

		/* Hack for buggy IIS that sets incorrect PATH_INFO */
		char *env_server_software = sapi_cgibin_getenv("SERVER_SOFTWARE", sizeof("SERVER_SOFTWARE")-1 TSRMLS_CC);
		if (env_server_software &&
			env_script_name &&
			env_path_info &&
			strncmp(env_server_software, "Microsoft-IIS", sizeof("Microsoft-IIS")-1) == 0 &&
			strncmp(env_path_info, env_script_name, strlen(env_script_name)) == 0
		) {
			env_path_info = _sapi_cgibin_putenv("ORIG_PATH_INFO", env_path_info TSRMLS_CC);
			env_path_info += strlen(env_script_name);
			if (*env_path_info == 0) {
				env_path_info = NULL;
			}
			env_path_info = _sapi_cgibin_putenv("PATH_INFO", env_path_info TSRMLS_CC);
		}

		if (CGIG(fix_pathinfo)) {
			struct stat st;
			char *real_path = NULL;
			char *env_redirect_url = sapi_cgibin_getenv("REDIRECT_URL", sizeof("REDIRECT_URL")-1 TSRMLS_CC);
			char *env_document_root = sapi_cgibin_getenv("DOCUMENT_ROOT", sizeof("DOCUMENT_ROOT")-1 TSRMLS_CC);
			char *orig_path_translated = env_path_translated;
			char *orig_path_info = env_path_info;
			char *orig_script_name = env_script_name;
			char *orig_script_filename = env_script_filename;
			int script_path_translated_len;

			if (!env_document_root && PG(doc_root)) {
				env_document_root = _sapi_cgibin_putenv("DOCUMENT_ROOT", PG(doc_root) TSRMLS_CC);
				/* fix docroot */
				TRANSLATE_SLASHES(env_document_root);
			}

			if (env_path_translated != NULL && env_redirect_url != NULL &&
 			    env_path_translated != script_path_translated &&
 			    strcmp(env_path_translated, script_path_translated) != 0) {
				/*
				 * pretty much apache specific.  If we have a redirect_url
				 * then our script_filename and script_name point to the
				 * php executable
				 */
				script_path_translated = env_path_translated;
				/* we correct SCRIPT_NAME now in case we don't have PATH_INFO */
				env_script_name = env_redirect_url;
			}

#ifdef __riscos__
			/* Convert path to unix format*/
			__riscosify_control |= __RISCOSIFY_DONT_CHECK_DIR;
			script_path_translated = __unixify(script_path_translated, 0, NULL, 1, 0);
#endif

			/*
			 * if the file doesn't exist, try to extract PATH_INFO out
			 * of it by stat'ing back through the '/'
			 * this fixes url's like /info.php/test
			 */
			if (script_path_translated &&
				(script_path_translated_len = strlen(script_path_translated)) > 0 &&
				(script_path_translated[script_path_translated_len-1] == '/' ||
#ifdef PHP_WIN32
				script_path_translated[script_path_translated_len-1] == '\\' ||
#endif
				(real_path = tsrm_realpath(script_path_translated, NULL TSRMLS_CC)) == NULL)
			) {
				char *pt = estrndup(script_path_translated, script_path_translated_len);
				int len = script_path_translated_len;
				char *ptr;

				while ((ptr = strrchr(pt, '/')) || (ptr = strrchr(pt, '\\'))) {
					*ptr = 0;
					if (stat(pt, &st) == 0 && S_ISREG(st.st_mode)) {
						/*
						 * okay, we found the base script!
						 * work out how many chars we had to strip off;
						 * then we can modify PATH_INFO
						 * accordingly
						 *
						 * we now have the makings of
						 * PATH_INFO=/test
						 * SCRIPT_FILENAME=/docroot/info.php
						 *
						 * we now need to figure out what docroot is.
						 * if DOCUMENT_ROOT is set, this is easy, otherwise,
						 * we have to play the game of hide and seek to figure
						 * out what SCRIPT_NAME should be
						 */
						int slen = len - strlen(pt);
						int pilen = env_path_info ? strlen(env_path_info) : 0;
						char *path_info = env_path_info ? env_path_info + pilen - slen : NULL;

						if (orig_path_info != path_info) {
							if (orig_path_info) {
								char old;

								_sapi_cgibin_putenv("ORIG_PATH_INFO", orig_path_info TSRMLS_CC);
								old = path_info[0];
								path_info[0] = 0;
								if (!orig_script_name ||
									strcmp(orig_script_name, env_path_info) != 0) {
									if (orig_script_name) {
										_sapi_cgibin_putenv("ORIG_SCRIPT_NAME", orig_script_name TSRMLS_CC);
									}
									SG(request_info).request_uri = _sapi_cgibin_putenv("SCRIPT_NAME", env_path_info TSRMLS_CC);
								} else {
									SG(request_info).request_uri = orig_script_name;
								}
								path_info[0] = old;
							}
							env_path_info = _sapi_cgibin_putenv("PATH_INFO", path_info TSRMLS_CC);
						}
						if (!orig_script_filename ||
							strcmp(orig_script_filename, pt) != 0) {
							if (orig_script_filename) {
								_sapi_cgibin_putenv("ORIG_SCRIPT_FILENAME", orig_script_filename TSRMLS_CC);
							}
							script_path_translated = _sapi_cgibin_putenv("SCRIPT_FILENAME", pt TSRMLS_CC);
						}
						TRANSLATE_SLASHES(pt);

						/* figure out docroot
						 * SCRIPT_FILENAME minus SCRIPT_NAME
						 */
						if (env_document_root) {
							int l = strlen(env_document_root);
							int path_translated_len = 0;
							char *path_translated = NULL;

							if (l && env_document_root[l - 1] == '/') {
								--l;
							}

							/* we have docroot, so we should have:
							 * DOCUMENT_ROOT=/docroot
							 * SCRIPT_FILENAME=/docroot/info.php
							 */

							/* PATH_TRANSLATED = DOCUMENT_ROOT + PATH_INFO */
							path_translated_len = l + (env_path_info ? strlen(env_path_info) : 0);
							path_translated = (char *) emalloc(path_translated_len + 1);
							memcpy(path_translated, env_document_root, l);
							if (env_path_info) {
								memcpy(path_translated + l, env_path_info, (path_translated_len - l));
							}
							path_translated[path_translated_len] = '\0';
							if (orig_path_translated) {
								_sapi_cgibin_putenv("ORIG_PATH_TRANSLATED", orig_path_translated TSRMLS_CC);
							}
							env_path_translated = _sapi_cgibin_putenv("PATH_TRANSLATED", path_translated TSRMLS_CC);
							efree(path_translated);
						} else if (	env_script_name &&
									strstr(pt, env_script_name)
						) {
							/* PATH_TRANSLATED = PATH_TRANSLATED - SCRIPT_NAME + PATH_INFO */
							int ptlen = strlen(pt) - strlen(env_script_name);
							int path_translated_len = ptlen + (env_path_info ? strlen(env_path_info) : 0);
							char *path_translated = NULL;

							path_translated = (char *) emalloc(path_translated_len + 1);
							memcpy(path_translated, pt, ptlen);
							if (env_path_info) {
								memcpy(path_translated + ptlen, env_path_info, path_translated_len - ptlen);
							}
							path_translated[path_translated_len] = '\0';
							if (orig_path_translated) {
								_sapi_cgibin_putenv("ORIG_PATH_TRANSLATED", orig_path_translated TSRMLS_CC);
							}
							env_path_translated = _sapi_cgibin_putenv("PATH_TRANSLATED", path_translated TSRMLS_CC);
							efree(path_translated);
						}
						break;
					}
				}
				if (!ptr) {
					/*
					 * if we stripped out all the '/' and still didn't find
					 * a valid path... we will fail, badly. of course we would
					 * have failed anyway... we output 'no input file' now.
					 */
					if (orig_script_filename) {
						_sapi_cgibin_putenv("ORIG_SCRIPT_FILENAME", orig_script_filename TSRMLS_CC);
					}
					script_path_translated = _sapi_cgibin_putenv("SCRIPT_FILENAME", NULL TSRMLS_CC);
					SG(sapi_headers).http_response_code = 404;
				}
				if (!SG(request_info).request_uri) {
					if (!orig_script_name ||
						strcmp(orig_script_name, env_script_name) != 0) {
						if (orig_script_name) {
							_sapi_cgibin_putenv("ORIG_SCRIPT_NAME", orig_script_name TSRMLS_CC);
						}
						SG(request_info).request_uri = _sapi_cgibin_putenv("SCRIPT_NAME", env_script_name TSRMLS_CC);
					} else {
						SG(request_info).request_uri = orig_script_name;
					}
				}
				if (pt) {
					efree(pt);
				}
			} else {
				/* make sure path_info/translated are empty */
				if (!orig_script_filename ||
					(script_path_translated != orig_script_filename &&
					strcmp(script_path_translated, orig_script_filename) != 0)) {
					if (orig_script_filename) {
						_sapi_cgibin_putenv("ORIG_SCRIPT_FILENAME", orig_script_filename TSRMLS_CC);
					}
					script_path_translated = _sapi_cgibin_putenv("SCRIPT_FILENAME", script_path_translated TSRMLS_CC);
				}
				if (env_redirect_url) {
					if (orig_path_info) {
						_sapi_cgibin_putenv("ORIG_PATH_INFO", orig_path_info TSRMLS_CC);
						_sapi_cgibin_putenv("PATH_INFO", NULL TSRMLS_CC);
					}
					if (orig_path_translated) {
						_sapi_cgibin_putenv("ORIG_PATH_TRANSLATED", orig_path_translated TSRMLS_CC);
						_sapi_cgibin_putenv("PATH_TRANSLATED", NULL TSRMLS_CC);
					}
				}
				if (env_script_name != orig_script_name) {
					if (orig_script_name) {
						_sapi_cgibin_putenv("ORIG_SCRIPT_NAME", orig_script_name TSRMLS_CC);
					}
					SG(request_info).request_uri = _sapi_cgibin_putenv("SCRIPT_NAME", env_script_name TSRMLS_CC);
				} else {
					SG(request_info).request_uri = env_script_name;
				}
				free(real_path);
			}
		} else {
			/* pre 4.3 behaviour, shouldn't be used but provides BC */
			if (env_path_info) {
				SG(request_info).request_uri = env_path_info;
			} else {
				SG(request_info).request_uri = env_script_name;
			}
			if (!CGIG(discard_path) && env_path_translated) {
				script_path_translated = env_path_translated;
			}
		}

		if (is_valid_path(script_path_translated)) {
			SG(request_info).path_translated = estrdup(script_path_translated);
		}

		SG(request_info).request_method = sapi_cgibin_getenv("REQUEST_METHOD", sizeof("REQUEST_METHOD")-1 TSRMLS_CC);
		/* FIXME - Work out proto_num here */
		SG(request_info).query_string = sapi_cgibin_getenv("QUERY_STRING", sizeof("QUERY_STRING")-1 TSRMLS_CC);
		SG(request_info).content_type = (content_type ? content_type : "" );
		SG(request_info).content_length = (content_length ? atoi(content_length) : 0);

		/* The CGI RFC allows servers to pass on unvalidated Authorization data */
		auth = sapi_cgibin_getenv("HTTP_AUTHORIZATION", sizeof("HTTP_AUTHORIZATION")-1 TSRMLS_CC);
		php_handle_auth_data(auth TSRMLS_CC);
	}
}
/* }}} */

/* {{{ php_cgi_globals_ctor
 */
static void php_cgi_globals_ctor(php_cgi_globals_struct *php_cgi_globals TSRMLS_DC)
{
	php_cgi_globals->rfc2616_headers = 0;
	php_cgi_globals->nph = 0;
	php_cgi_globals->check_shebang_line = 1;
	php_cgi_globals->force_redirect = 1;
	php_cgi_globals->redirect_status_env = NULL;
	php_cgi_globals->fix_pathinfo = 1;
	php_cgi_globals->discard_path = 0;
	php_cgi_globals->logging = 1;
#ifdef PHP_WIN32
	php_cgi_globals->impersonate = 0;
#endif
	zend_hash_init(&php_cgi_globals->user_config_cache, 0, NULL, (dtor_func_t) user_config_cache_entry_dtor, 1);
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(cgi)
{
#ifdef ZTS
	ts_allocate_id(&php_cgi_globals_id, sizeof(php_cgi_globals_struct), (ts_allocate_ctor) php_cgi_globals_ctor, NULL);
#else
	php_cgi_globals_ctor(&php_cgi_globals TSRMLS_CC);
#endif
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(cgi)
{
	zend_hash_destroy(&CGIG(user_config_cache));

	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(cgi)
{
	DISPLAY_INI_ENTRIES();
}
/* }}} */

static zend_module_entry cgi_module_entry = {
	STANDARD_MODULE_HEADER,
	"pvm",
	NULL,
	PHP_MINIT(cgi),
	PHP_MSHUTDOWN(cgi),
	NULL,
	NULL,
	PHP_MINFO(cgi),
	NO_VERSION_YET,
	STANDARD_MODULE_PROPERTIES
};

//taint functions
static void taint_pzval_unlock_func(zval *z, taint_free_op *should_free, int unref) /* {{{ */ {
    if (!Z_DELREF_P(z)) {
        Z_SET_REFCOUNT_P(z, 1);
        Z_UNSET_ISREF_P(z);
        should_free->var = z;
    } else {
        should_free->var = 0;
        if (unref && Z_ISREF_P(z) && Z_REFCOUNT_P(z) == 1) {
			should_free->is_ref = 1;
			Z_UNSET_ISREF_P(z);
        }
    }
} /* }}} */

static void taint_pzval_unlock_free_func(zval *z) /* {{{ */ {
    if (!Z_DELREF_P(z)) {
        zval_dtor(z);
        safe_free_zval_ptr(z);
    }
} /* }}} */

static void taint_pzval_lock_func(zval *z, taint_free_op *should_free) /* {{{ */ {
	if (should_free->type == IS_VAR) {
		Z_ADDREF_P(z);
		if (should_free->var && should_free->is_ref) {
			Z_SET_ISREF_P(z);
		}
	}
} /* }}} */

static void php_taint_get_cv_address(zend_compiled_variable *cv, zval ***ptr, temp_variable *Ts TSRMLS_DC) /* {{{ */ {
	zval *new_zval = &EG(uninitialized_zval);

	Z_ADDREF_P(new_zval);
	zend_hash_quick_update(EG(active_symbol_table), cv->name, cv->name_len+1, cv->hash_value, &new_zval, sizeof(zval *), (void **)ptr);
}
/* }}} */

static zval **php_taint_get_obj_zval_ptr_ptr_unused(TSRMLS_D) /* {{{ */ {
	if (EG(This)) {
		return &EG(This);
	} else {
		zend_error(E_ERROR, "Using $this when not in object context");
		return NULL;
	}
} /* }}} */

static void make_real_object(zval **object_ptr TSRMLS_DC)  /* {{{ */ {
	if (Z_TYPE_PP(object_ptr) == IS_NULL
		|| (Z_TYPE_PP(object_ptr) == IS_BOOL && Z_LVAL_PP(object_ptr) == 0)
		|| (Z_TYPE_PP(object_ptr) == IS_STRING && Z_STRLEN_PP(object_ptr) == 0)
	) {
		zend_error(E_STRICT, "Creating default object from empty value");

		SEPARATE_ZVAL_IF_NOT_REF(object_ptr);
		zval_dtor(*object_ptr);
		object_init(*object_ptr);
	}
} /* }}} */

static zval * php_taint_get_zval_ptr_var(znode *node, temp_variable *Ts, taint_free_op *should_free TSRMLS_DC) /* {{{ */ {
    zval *ptr = TAINT_TS(node->u.var).var.ptr;
    if (ptr) {
        TAINT_PZVAL_UNLOCK(ptr, should_free);
        return ptr;
    } else {
        temp_variable *T = (temp_variable *)((char *)Ts + node->u.var);
        zval *str = T->str_offset.str;

        /* string offset */
        ALLOC_ZVAL(ptr);
        T->str_offset.ptr = ptr;
        should_free->var = ptr;

        if (T->str_offset.str->type != IS_STRING
            || ((int)T->str_offset.offset < 0)
            || (T->str_offset.str->value.str.len <= (int)T->str_offset.offset)) {
            ptr->value.str.val = STR_EMPTY_ALLOC();
            ptr->value.str.len = 0;
        } else {
            char c = str->value.str.val[T->str_offset.offset];

            ptr->value.str.val = estrndup(&c, 1);
            ptr->value.str.len = 1;
        }
        TAINT_PZVAL_UNLOCK_FREE(str);

        ptr->refcount__gc = 1;
        ptr->is_ref__gc = 1;
        ptr->type = IS_STRING;
        return ptr;
    }
} /* }}} */

static zval * php_taint_get_zval_ptr_cv(znode *node, temp_variable *Ts TSRMLS_DC) /* {{{ */ {
	zval ***ptr = &TAINT_CV_OF(node->u.var);
	if (!*ptr) {
		zend_compiled_variable *cv = &TAINT_CV_DEF_OF(node->u.var);
		if (!EG(active_symbol_table) || zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)ptr) == FAILURE) {
			zend_error(E_NOTICE, "Undefined variable: %s", cv->name);
			return &EG(uninitialized_zval);
		}
	}
	return **ptr;
} /* }}} */

static zval * php_taint_get_zval_ptr_tmp(znode *node, temp_variable *Ts, taint_free_op *should_free TSRMLS_DC) /* {{{ */ {
	return should_free->var = &TAINT_TS(node->u.var).tmp_var;
} /* }}} */

static zval ** php_taint_get_zval_ptr_ptr_var(znode *node, temp_variable *Ts, taint_free_op *should_free TSRMLS_DC) /* {{{ */ {
	zval** ptr_ptr = TAINT_TS(node->u.var).var.ptr_ptr;

	if (ptr_ptr) {
		TAINT_PZVAL_UNLOCK(*ptr_ptr, should_free);
	} else {
		/* string offset */
		TAINT_PZVAL_UNLOCK(TAINT_TS(node->u.var).str_offset.str, should_free);
	}
	return ptr_ptr;
} /* }}} */

static zval **php_taint_get_zval_ptr_ptr_cv(znode *node, temp_variable *Ts, int type TSRMLS_DC) /* {{{ */ {
	zval ***ptr = &TAINT_CV_OF(node->u.var);
	
	if (!EG(active_symbol_table)) {
				zend_rebuild_symbol_table(TSRMLS_C);
	}
	
	if (!*ptr) {
		zend_compiled_variable *cv = &TAINT_CV_DEF_OF(node->u.var);
		if (!EG(active_symbol_table) 
				|| zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len+1, cv->hash_value, (void **) ptr )==FAILURE) {
			switch (type) {
				case BP_VAR_R:
				case BP_VAR_UNSET:
					zend_error(E_NOTICE, "Undefined variable: %s", cv->name);
					/* break missing intentionally */
				case BP_VAR_IS:
					return &EG(uninitialized_zval_ptr);
					break;
				case BP_VAR_RW:
					zend_error(E_NOTICE, "Undefined variable: %s", cv->name);
					/* break missing intentionally */
				case BP_VAR_W:
					php_taint_get_cv_address(cv, ptr, Ts TSRMLS_CC);
					break;
			}
		}
	}
	return *ptr;
} /* }}} */

static zval **php_taint_get_zval_ptr_ptr(znode *node, temp_variable *Ts, taint_free_op *should_free, int type TSRMLS_DC) /* {{{ */ {
	should_free->type = node->op_type;
	if (node->op_type == IS_CV) {
		should_free->var = 0;
		return php_taint_get_zval_ptr_ptr_cv(node, Ts, type TSRMLS_CC);
	} else if (node->op_type == IS_VAR) {
		return php_taint_get_zval_ptr_ptr_var(node, Ts, should_free TSRMLS_CC);
	} else {
		should_free->var = 0;
		return NULL;
	}
} /* }}} */

static zval *php_taint_get_zval_ptr(znode *node, temp_variable *Ts, taint_free_op *should_free, int type TSRMLS_DC) /* {{{ */ {
/*	should_free->is_var = 0; */
	switch (node->op_type) {
		case IS_CONST:
			should_free->var = 0;
			return &node->u.constant;
			break;
		case IS_TMP_VAR:
			should_free->var = TAINT_TMP_FREE(&TAINT_TS(node->u.var).tmp_var);
			return &TAINT_TS(node->u.var).tmp_var;
			break;
		case IS_VAR:
			return php_taint_get_zval_ptr_var(node, Ts, should_free TSRMLS_CC);
			break;
		case IS_UNUSED:
			should_free->var = 0;
			return NULL;
			break;
		case IS_CV:
			should_free->var = 0;
			return php_taint_get_zval_ptr_cv(node, Ts TSRMLS_CC);
			break;
		EMPTY_SWITCH_DEFAULT_CASE()
	}
	return NULL;
} /* }}} */

static zval **php_taint_fetch_dimension_address_inner(HashTable *ht, zval *dim, int dim_type, int type TSRMLS_DC) /* {{{ */ {
	zval **retval;
	char *offset_key;
	int offset_key_length;
	ulong hval;

	switch (dim->type) {
		case IS_NULL:
			offset_key = "";
			offset_key_length = 0;
			goto fetch_string_dim;

		case IS_STRING:
			offset_key = dim->value.str.val;
			offset_key_length = dim->value.str.len;
			
fetch_string_dim:
			if (zend_symtable_find(ht, offset_key, offset_key_length+1, (void **) &retval) == FAILURE) {
				switch (type) {
					case BP_VAR_R:
						zend_error(E_NOTICE, "Undefined index: %s", offset_key);
						/* break missing intentionally */
					case BP_VAR_UNSET:
					case BP_VAR_IS:
						retval = &EG(uninitialized_zval_ptr);
						break;
					case BP_VAR_RW:
						zend_error(E_NOTICE,"Undefined index: %s", offset_key);
						/* break missing intentionally */
					case BP_VAR_W: {
							zval *new_zval = &EG(uninitialized_zval);
							Z_ADDREF_P(new_zval);
						#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4)
							zend_symtable_update(ht, offset_key, offset_key_length+1, &new_zval, sizeof(zval *), (void **) &retval);
						#else
							zend_hash_quick_update(ht, offset_key, offset_key_length+1, hval, &new_zval, sizeof(zval *), (void **) &retval);
						#endif
						}
						break;
				}
			}
		#if 0
			}
		#endif
			break;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_STRICT, "Resource ID#%ld used as offset, casting to integer (%ld)", Z_LVAL_P(dim), Z_LVAL_P(dim));
			/* Fall Through */
		case IS_BOOL:
		case IS_LONG:
			hval = Z_LVAL_P(dim);
num_index:
			if (zend_hash_index_find(ht, hval, (void **) &retval) == FAILURE) {
				switch (type) {
					case BP_VAR_R:
						zend_error(E_NOTICE,"Undefined offset: %ld", hval);
						/* break missing intentionally */
					case BP_VAR_UNSET:
					case BP_VAR_IS:
						retval = &EG(uninitialized_zval_ptr);
						break;
					case BP_VAR_RW:
						zend_error(E_NOTICE,"Undefined offset: %ld", hval);
						/* break missing intentionally */
					case BP_VAR_W: {
						zval *new_zval = &EG(uninitialized_zval);

						Z_ADDREF_P(new_zval);
						zend_hash_index_update(ht, hval, &new_zval, sizeof(zval *), (void **) &retval);
					}
					break;
				}
			}
			break;

		default:
			zend_error(E_WARNING, "Illegal offset type");
			return (type == BP_VAR_W || type == BP_VAR_RW) ?
				&EG(error_zval_ptr) : &EG(uninitialized_zval_ptr);
	}
	return retval;
} /* }}} */

static void php_taint_fetch_dimension_address(temp_variable *result, zval **container_ptr, zval *dim, int dim_is_tmp_var, int type TSRMLS_DC)
{
	zval *container = *container_ptr;
	zval **retval;

	switch (Z_TYPE_P(container)) {

		case IS_ARRAY:
			if (type != BP_VAR_UNSET && Z_REFCOUNT_P(container)>1 && !Z_ISREF_P(container)) {
				SEPARATE_ZVAL(container_ptr);
				container = *container_ptr;
			}
fetch_from_array:
			if (dim == NULL) {
				zval *new_zval = &EG(uninitialized_zval);

				Z_ADDREF_P(new_zval);
				if (zend_hash_next_index_insert(Z_ARRVAL_P(container), &new_zval, sizeof(zval *), (void **) &retval) == FAILURE) {
					zend_error(E_WARNING, "Cannot add element to the array as the next element is already occupied");
					retval = &EG(error_zval_ptr);
					Z_DELREF_P(new_zval);
				}
			} else {
				retval = php_taint_fetch_dimension_address_inner(Z_ARRVAL_P(container), dim, 0, type TSRMLS_CC);
			}
			result->var.ptr_ptr = retval;
			Z_ADDREF_P(*retval);
			return;
			break;

		case IS_NULL:
			if (container == &EG(error_zval)) {
				result->var.ptr_ptr = &EG(error_zval_ptr);
				Z_ADDREF_P(EG(error_zval_ptr));
			} else if (type != BP_VAR_UNSET) {
convert_to_array:
				if (!Z_ISREF_P(container)) {
					SEPARATE_ZVAL(container_ptr);
					container = *container_ptr;
				}
				zval_dtor(container);
				array_init(container);
				goto fetch_from_array;
			} else {
				/* for read-mode only */
				result->var.ptr_ptr = &EG(uninitialized_zval_ptr);
				Z_ADDREF_P(EG(uninitialized_zval_ptr));
			}
			return;
			break;

		case IS_STRING: {
				zval tmp;

				if (type != BP_VAR_UNSET && Z_STRLEN_P(container)==0) {
					goto convert_to_array;
				}
				if (dim == NULL) {
					zend_error(E_ERROR, "[] operator not supported for strings");
					return;
				}

				if (Z_TYPE_P(dim) != IS_LONG) {

					switch(Z_TYPE_P(dim)) {
						/* case IS_LONG: */
						case IS_STRING:
							if (IS_LONG == is_numeric_string(Z_STRVAL_P(dim), Z_STRLEN_P(dim), NULL, NULL, -1)) {
								break;
							}
							if (type != BP_VAR_UNSET) {
								zend_error(E_WARNING, "Illegal string offset '%s'", dim->value.str.val);
							}

							break;
						case IS_DOUBLE:
						case IS_NULL:
						case IS_BOOL:
							zend_error(E_NOTICE, "String offset cast occurred");
							break;
						default:
							zend_error(E_WARNING, "Illegal offset type");
							break;
					}

					tmp = *dim;
					zval_copy_ctor(&tmp);
					convert_to_long(&tmp);
					dim = &tmp;
				}
				if (type != BP_VAR_UNSET) {
					SEPARATE_ZVAL_IF_NOT_REF(container_ptr);
				}
				container = *container_ptr;
				result->str_offset.str = container;
				Z_ADDREF_P(container);
				result->str_offset.offset = Z_LVAL_P(dim);
				result->str_offset.ptr_ptr = NULL;
				return;
			}
			break;

		case IS_OBJECT:
			if (!Z_OBJ_HT_P(container)->read_dimension) {
				zend_error(E_ERROR, "Cannot use object as array");
				return;
			} else {
				zval *overloaded_result;
			#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4)
				if (dim_is_tmp_var) {
			#else
				if (dim_type == IS_TMP_VAR) {
			#endif
					zval *orig = dim;
					MAKE_REAL_ZVAL_PTR(dim);
					ZVAL_NULL(orig);
				}
			#if 0
				}
			#endif
				overloaded_result = Z_OBJ_HT_P(container)->read_dimension(container, dim, type TSRMLS_CC);

				if (overloaded_result) {
					if (!Z_ISREF_P(overloaded_result)) {
						if (Z_REFCOUNT_P(overloaded_result) > 0) {
							zval *tmp = overloaded_result;

							ALLOC_ZVAL(overloaded_result);
							/* ZVAL_COPY_VALUE(overloaded_result, tmp); */
							overloaded_result->value = tmp->value;
							Z_TYPE_P(overloaded_result) = Z_TYPE_P(tmp);
							zval_copy_ctor(overloaded_result);
							Z_UNSET_ISREF_P(overloaded_result);
							Z_SET_REFCOUNT_P(overloaded_result, 0);
						}
						if (Z_TYPE_P(overloaded_result) != IS_OBJECT) {
							zend_class_entry *ce = Z_OBJCE_P(container);
							zend_error(E_NOTICE, "Indirect modification of overloaded element of %s has no effect", ce->name);
						}
					}
					retval = &overloaded_result;
				} else {
					retval = &EG(error_zval_ptr);
				}
				TAINT_AI_SET_PTR(result->var, *retval);
				Z_ADDREF_P(*retval);
				if (dim_is_tmp_var) {
					zval_ptr_dtor(&dim);
				}
			#if 0
				}
			#endif
			}
			return;
			break;

		case IS_BOOL:
			if (type != BP_VAR_UNSET && Z_LVAL_P(container)==0) {
				goto convert_to_array;
			}
			/* break missing intentionally */

		default:
			if (type == BP_VAR_UNSET) {
				zend_error(E_WARNING, "Cannot unset offset in a non-array variable");
				TAINT_AI_SET_PTR(result->var, EG(uninitialized_zval_ptr));
				Z_ADDREF_P(&EG(uninitialized_zval));
			} else {
				zend_error(E_WARNING, "Cannot use a scalar value as an array");
				result->var.ptr_ptr = &EG(error_zval_ptr);
				Z_ADDREF_P(EG(error_zval_ptr));
			}
			break;
	}
#if 0
}
#endif
}

static int php_taint_binary_assign_op_obj_helper(int (*binary_op)(zval *result, zval *op1, zval *op2 TSRMLS_DC), ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
	zend_op *opline = execute_data->opline;
	zend_op *op_data = opline+1;
	taint_free_op free_op1 = {0}, free_op2 = {0}, free_op_data1 = {0};
	zval **object_ptr = NULL, *object = NULL, *property = NULL;
	int have_get_ptr = 0;
	uint tainted = 0;

	zval *value = php_taint_get_zval_ptr(&op_data->op1, execute_data->Ts, &free_op_data1, BP_VAR_R TSRMLS_CC);
	zval **retval = &TAINT_T(TAINT_RESULT_VAR(opline)).var.ptr;

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			object_ptr = php_taint_get_zval_ptr_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			if (!object_ptr) {
				zend_error(E_ERROR, "Cannot use string offset as an object");
				return 0;
			}
			break;
		case IS_CV:
			object_ptr = php_taint_get_zval_ptr_ptr_cv(&opline->op1, execute_data->Ts, BP_VAR_W TSRMLS_CC);
			break;
		case IS_UNUSED:
			object_ptr = php_taint_get_obj_zval_ptr_ptr_unused(TSRMLS_C);
			break;
		default:
			/* do nothing */
			break;
	}
	
	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			property = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_VAR:
			property = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_CV:
			property = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
			property = TAINT_OP2_CONSTANT_PTR(opline);
			break;
		case IS_UNUSED:
			property = NULL;
			break;
		default:
			/* do nothing */
			break;
	}
	
	TAINT_T(TAINT_RESULT_VAR(opline)).var.ptr_ptr = NULL;
	make_real_object(object_ptr TSRMLS_CC);
	object = *object_ptr;

	if (Z_TYPE_P(object) != IS_OBJECT) {
		zend_error(E_WARNING, "Attempt to assign property of non-object");
		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				zval_dtor(free_op2.var);
				break;
			case IS_VAR:
				if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
				break;
			case IS_CV:
			case IS_CONST:
			case IS_UNUSED:
			default:
				/* do nothing */
				break;
		}
		TAINT_FREE_OP(free_op_data1);

		if (TAINT_RETURN_VALUE_USED(opline)) {
			*retval = EG(uninitialized_zval_ptr);
			Z_ADDREF_P(*retval);
		}
	} else {
		/* here we are sure we are dealing with an object */
		if (IS_TMP_VAR == TAINT_OP2_TYPE(opline)) {
			MAKE_REAL_ZVAL_PTR(property);
		}

		/* here property is a string */
		if (opline->extended_value == ZEND_ASSIGN_OBJ
			&& Z_OBJ_HT_P(object)->get_property_ptr_ptr) {
			zval **zptr = Z_OBJ_HT_P(object)->get_property_ptr_ptr(object, property TSRMLS_CC);
			if (zptr != NULL) { 			/* NULL means no success in getting PTR */
				if ((*zptr && IS_STRING == Z_TYPE_PP(zptr) && Z_STRLEN_PP(zptr) && PHP_TAINT_POSSIBLE((*zptr))) 
					|| (value && IS_STRING == Z_TYPE_P(value) && Z_STRLEN_P(value) && PHP_TAINT_POSSIBLE(value))){
					tainted = (*zptr)->taint|value->taint;
				}
				
				SEPARATE_ZVAL_IF_NOT_REF(zptr);
				have_get_ptr = 1;
				
				binary_op(*zptr, *zptr, value TSRMLS_CC);
				if (tainted && IS_STRING == Z_TYPE_PP(zptr) && Z_STRLEN_PP(zptr)) {
					Z_TAINT_PP(zptr, tainted);
				}
				if (TAINT_RETURN_VALUE_USED(opline)) {
					*retval = *zptr;
					Z_ADDREF_P(*retval);
				}
			}
		}

		if (!have_get_ptr) {
			zval *z = NULL;

			switch (opline->extended_value) {
				case ZEND_ASSIGN_OBJ:
					if (Z_OBJ_HT_P(object)->read_property) {
					#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4)
						z = Z_OBJ_HT_P(object)->read_property(object, property, BP_VAR_R TSRMLS_CC);
					#else
						z = Z_OBJ_HT_P(object)->read_property(object, property, BP_VAR_R, ((TAINT_OP2_TYPE(opline) == IS_CONST) ? opline->op2.literal : NULL) TSRMLS_CC);
					#endif
					}
					break;
				case ZEND_ASSIGN_DIM:
					if (Z_OBJ_HT_P(object)->read_dimension) {
						z = Z_OBJ_HT_P(object)->read_dimension(object, property, BP_VAR_R TSRMLS_CC);
					}
					break;
			}
			if (z) {
				if (Z_TYPE_P(z) == IS_OBJECT && Z_OBJ_HT_P(z)->get) {
					zval *value = Z_OBJ_HT_P(z)->get(z TSRMLS_CC);

					if (Z_REFCOUNT_P(z) == 0) {
						zval_dtor(z);
						FREE_ZVAL(z);
					}
					z = value;
				}
				Z_ADDREF_P(z);
				if ((z && IS_STRING == Z_TYPE_P(z) && Z_STRLEN_P(z) && PHP_TAINT_POSSIBLE(z)) 
					|| (value && IS_STRING == Z_TYPE_P(value) && Z_STRLEN_P(value) && PHP_TAINT_POSSIBLE(value))) {
					tainted = z->taint|value->taint;
				}
				
				SEPARATE_ZVAL_IF_NOT_REF(&z);
				binary_op(z, z, value TSRMLS_CC);
				if (tainted && IS_STRING == Z_TYPE_P(z) && Z_STRLEN_P(z)) {
					Z_TAINT_P(z, tainted);
				}
				
				switch (opline->extended_value) {
					case ZEND_ASSIGN_OBJ:
						Z_OBJ_HT_P(object)->write_property(object, property, z TSRMLS_CC);
						break;
					case ZEND_ASSIGN_DIM:
						Z_OBJ_HT_P(object)->write_dimension(object, property, z TSRMLS_CC);
						break;
				}
				if (TAINT_RETURN_VALUE_USED(opline)) {
					*retval = z;
					Z_ADDREF_P(*retval);
				}
				zval_ptr_dtor(&z);
			} else {
				zend_error(E_WARNING, "Attempt to assign property of non-object");
				if (TAINT_RETURN_VALUE_USED(opline)) {
					*retval = EG(uninitialized_zval_ptr);
					Z_ADDREF_P(*retval);
				}
			}
		}

		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				zval_ptr_dtor(&property);
				break;
			case IS_VAR:
				if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
				break;
			case IS_CV:
			case IS_CONST:
			case IS_UNUSED:
			default:
				/* do nothing */
				break;
		}
		
		TAINT_FREE_OP(free_op_data1);
	}

	if (IS_VAR == TAINT_OP1_TYPE(opline) && free_op1.var) {zval_ptr_dtor(&free_op1.var);};
	/* assign_obj has two opcodes! */
	execute_data->opline++;
	execute_data->opline++;
	return ZEND_USER_OPCODE_CONTINUE; 
} /* }}} */ 

static int php_taint_binary_assign_op_helper(int (*binary_op)(zval *result, zval *op1, zval *op2 TSRMLS_DC), ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
	zend_op *opline = execute_data->opline;
	taint_free_op free_op1 = {0}, free_op2 = {0}, free_op_data2 = {0}, free_op_data1 = {0};
	zval **var_ptr = NULL, **object_ptr = NULL, *value = NULL;
	zend_bool increment_opline = 0;
	uint tainted = 0;

	switch (opline->extended_value) {
		case ZEND_ASSIGN_OBJ:
			return php_taint_binary_assign_op_obj_helper(binary_op, ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
			break;
		case ZEND_ASSIGN_DIM: {
			switch (TAINT_OP1_TYPE(opline)) {
				case IS_VAR:
					object_ptr = php_taint_get_zval_ptr_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
					if (object_ptr && !(free_op1.var != NULL)) {
						Z_ADDREF_P(*object_ptr);  /* undo the effect of get_obj_zval_ptr_ptr() */
					}
					break;
				case IS_CV:
				#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4)
					object_ptr = php_taint_get_zval_ptr_ptr_cv(&opline->op1, execute_data->Ts, BP_VAR_W TSRMLS_CC);
				#else
					object_ptr = php_taint_get_zval_ptr_ptr_cv(opline->op1.var, BP_VAR_W TSRMLS_CC);
				#endif
					break;
				case IS_UNUSED:
					object_ptr = php_taint_get_obj_zval_ptr_ptr_unused(TSRMLS_C);
					if (object_ptr) {
						Z_ADDREF_P(*object_ptr);  /* undo the effect of get_obj_zval_ptr_ptr() */
					}
					break;
				default:
					/* do nothing */
					break;
			}
			
			if (object_ptr && Z_TYPE_PP(object_ptr) == IS_OBJECT) {
				return php_taint_binary_assign_op_obj_helper(binary_op, ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
			} else {
				zend_op *op_data = opline+1;

				zval *dim;

				switch(TAINT_OP2_TYPE(opline)) {
					case IS_TMP_VAR:
						dim = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
						break;
					case IS_VAR:
						dim = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
						break;
					case IS_CV:
						dim = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
						break;
					case IS_CONST:
						dim = TAINT_OP2_CONSTANT_PTR(opline);
						break;
					case IS_UNUSED:
						dim = NULL;
						break;
					default:
						/* do nothing */
						break;
				}
				
				if (TAINT_OP2_TYPE(opline) == IS_TMP_VAR) {
					php_taint_fetch_dimension_address(&TAINT_T(TAINT_OP2_VAR(op_data)), object_ptr, dim, 1, BP_VAR_RW TSRMLS_CC);
				} else {
					php_taint_fetch_dimension_address(&TAINT_T(TAINT_OP2_VAR(op_data)), object_ptr, dim, 0, BP_VAR_RW TSRMLS_CC);
				}
				value = php_taint_get_zval_ptr(&op_data->op1, execute_data->Ts, &free_op_data1, BP_VAR_R TSRMLS_CC);
				var_ptr = php_taint_get_zval_ptr_ptr(&op_data->op2, execute_data->Ts, &free_op_data2, BP_VAR_RW TSRMLS_CC);
				increment_opline = 1;
			}
		}
		break;
	default:
		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				value = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_VAR:
				value = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_CV:
				value = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
				break;
			case IS_CONST:
				value = TAINT_OP2_CONSTANT_PTR(opline);
				break;
			case IS_UNUSED:
				value = NULL;
				break;
			default:
				/* do nothing */
				break;
		}

		switch (TAINT_OP1_TYPE(opline)) {
			case IS_VAR:
				var_ptr = php_taint_get_zval_ptr_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
				break;
			case IS_CV:
			#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4)
				var_ptr = php_taint_get_zval_ptr_ptr_cv(&opline->op1, execute_data->Ts, BP_VAR_RW TSRMLS_CC);
			#else
				var_ptr = php_taint_get_zval_ptr_ptr_cv(opline->op1.var, BP_VAR_RW TSRMLS_CC);
			#endif
				break;
			case IS_UNUSED:
				var_ptr = NULL;
				break;
			default:
				/* do nothing */
				break;
		}
		/* do nothing */
		break;
	}

	if (!var_ptr) {
		zend_error(E_ERROR, "Cannot use assign-op operators with overloaded objects nor string offsets");
		return 0;
	}

	if (*var_ptr == EG(error_zval_ptr)) {
		if (TAINT_RETURN_VALUE_USED(opline)) {
			TAINT_T(TAINT_RESULT_VAR(opline)).var.ptr_ptr = &EG(uninitialized_zval_ptr);
			Z_ADDREF_P(*TAINT_T(TAINT_RESULT_VAR(opline)).var.ptr_ptr);
			TAINT_AI_USE_PTR(TAINT_T(TAINT_RESULT_VAR(opline)).var);
		}
		
		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				zval_dtor(free_op2.var);
				break;
			case IS_VAR:
				if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
				break;
			case IS_CV:
			case IS_CONST:
			case IS_UNUSED:
			default:
				/* do nothing */
				break;
		}
		
		if (IS_VAR == TAINT_OP1_TYPE(opline) && free_op1.var) {zval_ptr_dtor(&free_op1.var);};
		if (increment_opline) {
			execute_data->opline++;
		}
		execute_data->opline++;
	}

	if ((*var_ptr && IS_STRING == Z_TYPE_PP(var_ptr) && Z_STRLEN_PP(var_ptr) && PHP_TAINT_POSSIBLE((*var_ptr)))
		|| (value && IS_STRING == Z_TYPE_P(value) && Z_STRLEN_P(value) && PHP_TAINT_POSSIBLE(value))) {
		tainted = (*var_ptr)->taint|value->taint;
	}
	
	SEPARATE_ZVAL_IF_NOT_REF(var_ptr);

	if(Z_TYPE_PP(var_ptr) == IS_OBJECT && Z_OBJ_HANDLER_PP(var_ptr, get)
	   && Z_OBJ_HANDLER_PP(var_ptr, set)) {
		/* proxy object */
		zval *objval = Z_OBJ_HANDLER_PP(var_ptr, get)(*var_ptr TSRMLS_CC);
		Z_ADDREF_P(objval);
		if ((objval && IS_STRING == Z_TYPE_P(objval) && Z_STRLEN_P(objval) && PHP_TAINT_POSSIBLE(objval))
			|| (value && IS_STRING == Z_TYPE_P(value) && Z_STRLEN_P(value) && PHP_TAINT_POSSIBLE(value))) {
			tainted = objval->taint|value->taint;
		}
		binary_op(objval, objval, value TSRMLS_CC);
		if (tainted && IS_STRING == Z_TYPE_P(objval) && Z_STRLEN_P(objval)) {
			Z_TAINT_P(objval, tainted);
		}
		
		Z_OBJ_HANDLER_PP(var_ptr, set)(var_ptr, objval TSRMLS_CC);
		zval_ptr_dtor(&objval);
	} else {
		binary_op(*var_ptr, *var_ptr, value TSRMLS_CC);
		if (tainted && IS_STRING == Z_TYPE_PP(var_ptr) && Z_STRLEN_PP(var_ptr)) {
			Z_TAINT_PP(var_ptr, tainted);
		}
	}

	if (TAINT_RETURN_VALUE_USED(opline)) {
		TAINT_T(TAINT_RESULT_VAR(opline)).var.ptr_ptr = var_ptr;
		Z_ADDREF_P(*var_ptr);
		TAINT_AI_USE_PTR(TAINT_T(TAINT_RESULT_VAR(opline)).var);
	}

	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op2.var);
			break;
		case IS_VAR:
			if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
			break;
		case IS_CV:
		case IS_CONST:
		case IS_UNUSED:
		default:
			/* do nothing */
			break;
	}
	
	if (increment_opline) {
		execute_data->opline++;
		TAINT_FREE_OP(free_op_data1);
		TAINT_FREE_OP_VAR_PTR(free_op_data2);
	}
	if (IS_VAR == TAINT_OP1_TYPE(opline) && free_op1.var) {zval_ptr_dtor(&free_op1.var);};
	
	execute_data->opline++;
	return ZEND_USER_OPCODE_CONTINUE; 
} /* }}} */

//hooked functions
static int my_include_or_eval(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL;
	taint_free_op free_op1 = {0};

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = TAINT_T(TAINT_OP1_VAR(opline)).var.ptr;
			break;
		case IS_CV: {
				zval **t = TAINT_CV_OF(TAINT_OP1_VAR(opline));
				if (t && *t) {
					op1 = *t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP1_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op1 = *t;
					}
				}
		    }
			break;
		case IS_CONST:
	 		op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}

	if ((op1 && IS_STRING == Z_TYPE_P(op1) && PHP_TAINT_POSSIBLE(op1)))
		switch (Z_LVAL(opline->op2.u.constant)) {
			case ZEND_INCLUDE_ONCE:
				error_output("function.include_once" TSRMLS_CC, opline->lineno, "File path contains data that might be tainted");
				break;
			case ZEND_REQUIRE_ONCE:
				error_output("function.require_once" TSRMLS_CC, opline->lineno, "File path contains data that might be tainted");
				break;
			case ZEND_INCLUDE:
				error_output("function.include" TSRMLS_CC, opline->lineno, "File path contains data that might be tainted");
				break;
			case ZEND_REQUIRE:
				error_output("function.require" TSRMLS_CC, opline->lineno, "File path contains data that might be tainted");
				break;
			case ZEND_EVAL:
				error_output("function.eval" TSRMLS_CC, opline->lineno, "Eval code contains data that might be tainted");
				break;
		}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_concat(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *op2 = NULL, *result;
	taint_free_op free_op1 = {0}, free_op2 = {0};
	uint tainted = 0;

	result = &TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var;
	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}

	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			op2 = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_VAR:
			op2 = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_CV:
			op2 = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op2 = TAINT_OP2_CONSTANT_PTR(opline);
			break;
	}

	if ((op1 && IS_STRING == Z_TYPE_P(op1) && PHP_TAINT_POSSIBLE(op1))
			|| (op2 && IS_STRING == Z_TYPE_P(op2) && PHP_TAINT_POSSIBLE(op2))) {
		tainted = op1->taint|op2->taint;
	}

	concat_function(result, op1, op2 TSRMLS_CC);

	if (tainted && IS_STRING == Z_TYPE_P(result)) {
		Z_TAINT_P(result, tainted);
	}

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op1.var);
			break;
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}

	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op2.var);
			break;
		case IS_VAR:
			if (free_op2.var) {
				zval_ptr_dtor(&free_op2.var);
			}
			break;
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */


static int my_assign_concat(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    return php_taint_binary_assign_op_helper(concat_function, ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
} /* }}} */

static int my_add_char(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *result;
	taint_free_op free_op1 = {0};
	uint tainted = 0;

	result = &TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var;

	op1 = result;
	if (TAINT_OP1_TYPE(opline) == IS_UNUSED) {
		/* Initialize for erealloc in add_string_to_string */
		Z_STRVAL_P(op1) = NULL;
		Z_STRLEN_P(op1) = 0;
		Z_TYPE_P(op1) = IS_STRING;
		INIT_PZVAL(op1);
	} else {

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}
	}

	if ((op1 && IS_STRING == Z_TYPE_P(op1)
				&& Z_STRVAL_P(op1)
				&& PHP_TAINT_POSSIBLE(op1))) {
		tainted = op1->taint;
	}

	add_char_to_string(result, op1, TAINT_OP2_CONSTANT_PTR(opline));

	if (tainted && IS_STRING == Z_TYPE_P(result)) {
		Z_TAINT_P(result, tainted);
	}

	/* FREE_OP is missing intentionally here - we're always working on the same temporary variable */
	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int my_add_var(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *op2 = NULL, *result;
	taint_free_op free_op1 = {0}, free_op2 = {0};
	uint tainted = 0;
	zval var_copy;
	int use_copy = 0;

	result = &TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var;

	op1 = result;
	if (TAINT_OP1_TYPE(opline) == IS_UNUSED) {
		/* Initialize for erealloc in add_string_to_string */
		Z_STRVAL_P(op1) = NULL;
		Z_STRLEN_P(op1) = 0;
		Z_TYPE_P(op1) = IS_STRING;
		INIT_PZVAL(op1);
	} else {

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}
	}

	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			op2 = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_VAR:
			op2 = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
			break;
		case IS_CV:
			op2 = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op2 = TAINT_OP2_CONSTANT_PTR(opline);
			break;
	}

	if ((op1 && IS_STRING == Z_TYPE_P(op1)
				&& Z_STRVAL_P(op1)
				&& PHP_TAINT_POSSIBLE(op1))
			|| (op2 && IS_STRING == Z_TYPE_P(op2)
				&& Z_STRVAL_P(op2)
				&& PHP_TAINT_POSSIBLE(op2))) {
		tainted = op1->taint|op2->taint;
	}

	if (Z_TYPE_P(op2) != IS_STRING) {
		zend_make_printable_zval(op2, &var_copy, &use_copy);
		if (use_copy) {
			op2 = &var_copy;
		}
	}

	add_string_to_string(result, op1, op2);

	if (use_copy) {
		zval_dtor(op2);
	}

	if (tainted && IS_STRING == Z_TYPE_P(result)) {
		Z_TAINT_P(result, tainted);
	}

	/* original comment, possibly problematic:
	 * FREE_OP is missing intentionally here - we're always working on the same temporary variable
	 * (Zeev):  I don't think it's problematic, we only use variables
	 * which aren't affected by FREE_OP(Ts, )'s anyway, unless they're
	 * string offsets or overloaded objects
	 */
	switch(TAINT_OP2_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op2.var);
			break;
		case IS_VAR:
			if (free_op2.var) {
				zval_ptr_dtor(&free_op2.var);
			}
			break;
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int my_add_string(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *result;
	taint_free_op free_op1 = {0};
	uint tainted = 0;

	result = &TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var;

	op1 = result;
	if (TAINT_OP1_TYPE(opline) == IS_UNUSED) {
		/* Initialize for erealloc in add_string_to_string */
		Z_STRVAL_P(op1) = NULL;
		Z_STRLEN_P(op1) = 0;
		Z_TYPE_P(op1) = IS_STRING;
		INIT_PZVAL(op1);
	} else {
	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
	 		op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}
	}

	if ((op1 && IS_STRING == Z_TYPE_P(op1) &&
		Z_STRVAL_P(op1) &&
		PHP_TAINT_POSSIBLE(op1))) {
		tainted = op1->taint;
	}

	add_string_to_string(result, op1, TAINT_OP2_CONSTANT_PTR(opline));

	if (tainted && IS_STRING == Z_TYPE_P(result)) {
		Z_TAINT_P(result, tainted);
	}

	/* FREE_OP is missing intentionally here - we're always working on the same temporary variable */
	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

//special func call
static void php_taint_fcall_check(ZEND_OPCODE_HANDLER_ARGS, zend_op *opline, char *fname, int len) /* {{{ */ {
	if (fname) {
	    void **p = EG(argument_stack)->top;
		int arg_count = opline->extended_value;

		if (!arg_count) {
			return;
		}

		do {
			//file operate
			if (strncmp("fopen", fname, len) == 0) {
				zval *el;
				el = *((zval **) (p - arg_count));
				if (el && IS_STRING == Z_TYPE_P(el) && PHP_TAINT_POSSIBLE(el)) {
					error_output("function.fopen" TSRMLS_CC, opline->lineno, "First argument contains data that might be tainted");
				}
				break;
			}

			if (strncmp("file_put_contents", fname, len) == 0
				   || strncmp("fwrite", fname, len) == 0) {
				if (arg_count > 1) {
					zval *fp, *str;

					fp = *((zval **) (p - arg_count));
					str = *((zval **) (p - (arg_count - 1)));

					if (fp && IS_RESOURCE == Z_TYPE_P(fp)) {
						break;
					} else if (fp && IS_STRING == Z_TYPE_P(fp)) {
						if (strncasecmp("php://output", Z_STRVAL_P(fp), Z_STRLEN_P(fp))) {
							break;
						}
					}
					if (str && IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
						error_output("function.file_put_contents" TSRMLS_CC, opline->lineno, "Second argument contains data that might be tainted");
					}
				}
				break;
			}

			//execute oprate
			if (strncmp("passthru", fname, len) == 0
					|| strncmp("system", fname, len) == 0
					|| strncmp("exec", fname, len) == 0
					|| strncmp("shell_exec", fname, len) == 0
					|| strncmp("proc_open", fname, len) == 0 ) {
				zval *el;
				el = *((zval **) (p - arg_count));
				if (el && IS_STRING == Z_TYPE_P(el) && PHP_TAINT_POSSIBLE(el)) {
					error_output(fname TSRMLS_CC, opline->lineno, "CMD statement contains data that might be tainted");
				}
				break;
			}
		} while (0);
	}
} /* }}} */

static int my_do_fcall(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *fname = TAINT_OP1_CONSTANT_PTR(opline);

	zend_function *old_func = EG(current_execute_data)->function_state.function;
	if (zend_hash_find(EG(function_table), fname->value.str.val, fname->value.str.len+1, (void **)&EG(current_execute_data)->function_state.function) == SUCCESS) {
		if (!EG(current_execute_data)->function_state.function->common.scope) {
			php_taint_fcall_check(ZEND_OPCODE_HANDLER_ARGS_PASSTHRU, opline, Z_STRVAL_P(fname), Z_STRLEN_P(fname));
		}
	}
	EG(current_execute_data)->function_state.function = old_func;

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_do_fcall_by_name(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zend_class_entry *scope = execute_data->fbc->common.scope;
	char *fname = (char *)(execute_data->fbc->common.function_name);

	zend_function *old_func = EG(current_execute_data)->function_state.function;
	EG(current_execute_data)->function_state.function = execute_data->fbc;
	if (!scope) {
		php_taint_fcall_check(ZEND_OPCODE_HANDLER_ARGS_PASSTHRU, opline, fname, strlen(fname));
	}
	EG(current_execute_data)->function_state.function = old_func;

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_init_fcall_by_name(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval **op1 = NULL, **op2 = NULL;

	switch (TAINT_OP2_TYPE(opline)) {
		case IS_VAR:
			op2 = TAINT_T(TAINT_OP2_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP2_VAR(opline));
				if (t && *t) {
					op2 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP2_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op2 = t;
					}
				}
			}
			break;
		default:
			return ZEND_USER_OPCODE_DISPATCH;
			break;
	}

	if (op2 || *op2 != &EG(error_zval) || Z_TYPE_PP(op2) == IS_STRING || Z_STRLEN_PP(op2) || PHP_TAINT_POSSIBLE((*op2))) {
		error_output("call by var" TSRMLS_CC, opline->lineno, "The function name might be tainted");
	}
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_assign(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval **op1 = NULL, **op2 = NULL;

	switch (TAINT_OP2_TYPE(opline)) {
		case IS_VAR:
			op2 = TAINT_T(TAINT_OP2_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP2_VAR(opline));
				if (t && *t) {
					op2 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP2_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op2 = t;
					}
				}
			}
			break;
		default:
			return ZEND_USER_OPCODE_DISPATCH;
			break;
	}

	if (!op2 || *op2 == &EG(error_zval) || Z_TYPE_PP(op2) != IS_STRING || !Z_STRLEN_PP(op2) || !PHP_TAINT_POSSIBLE((*op2))) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			op1 = TAINT_T(TAINT_OP1_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP1_VAR(opline));
				if (t && *t) {
					op1 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP1_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op1 = t;
					}
				}
			}
			break;
	}

	if (op1 && *op1 != &EG(error_zval) && Z_TYPE_PP(op1) != IS_OBJECT 
			&& PZVAL_IS_REF(*op1) && IS_TMP_VAR != TAINT_OP2_TYPE(opline)) {
		zval garbage = **op1;
		zend_uint refcount = Z_REFCOUNT_PP(op1);

		**op1 = **op2;
		Z_SET_REFCOUNT_P(*op1, refcount);
		Z_SET_ISREF_PP(op1);
		zval_copy_ctor(*op1);
		zval_dtor(&garbage);
		Z_TAINT_PP(op1, TAINT_ALL);

		execute_data->opline++;
		return ZEND_USER_OPCODE_CONTINUE;
	} else if (PZVAL_IS_REF(*op2) && Z_REFCOUNT_PP(op2) > 1) {
		SEPARATE_ZVAL(op2);
		Z_TAINT_PP(op2, TAINT_ALL);
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_assign_ref(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval **op1 = NULL, **op2 = NULL;

	if (opline->extended_value == ZEND_RETURNS_FUNCTION && TAINT_OP2_TYPE(opline) == IS_VAR) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	switch (TAINT_OP2_TYPE(opline)) {
		case IS_VAR:
			op2 = TAINT_T(TAINT_OP2_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP2_VAR(opline));
				if (t && *t) {
					op2 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP2_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op2 = t;
					}
				}
			}
			break;
	}

	if (!op2 || *op2 == &EG(error_zval) || IS_STRING != Z_TYPE_PP(op2)
			|| PZVAL_IS_REF(*op2) || !Z_STRLEN_PP(op2) || !PHP_TAINT_POSSIBLE((*op2))) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			op1 = TAINT_T(TAINT_OP1_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP1_VAR(opline));
				if (t && *t) {
					op1 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP1_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op1 = t;
					}
				}
			}
			break;
	}

	if (op1 && *op1 == &EG(error_zval)) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	if (!op1 || *op1 != *op2) {
		SEPARATE_ZVAL(op2);
		/* TODO: free the op2 if it is a var, now ignore the memleak */
		Z_ADDREF_P(*op2);
		Z_SET_ISREF_PP(op2);
		Z_TAINT_PP(op1, TAINT_ALL);
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_send_ref(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval **op1 = NULL;
	taint_free_op free_op1 = {0};

	if (execute_data->function_state.function->type == ZEND_INTERNAL_FUNCTION
			&& !ARG_SHOULD_BE_SENT_BY_REF(execute_data->fbc, TAINT_OP_LINENUM(opline->op2))) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			op1 = TAINT_T(TAINT_OP1_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP1_VAR(opline));
				if (t && *t) {
					op1 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP1_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op1 = t;
					}
				}
			}
			break;
	}

	if (!op1 || *op1 == &EG(error_zval) || *op1 == &EG(uninitialized_zval) || IS_STRING != Z_TYPE_PP(op1) 
			 || PZVAL_IS_REF(*op1) || Z_REFCOUNT_PP(op1) < 2 || !Z_STRLEN_PP(op1) || !PHP_TAINT_POSSIBLE((*op1))) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	SEPARATE_ZVAL_TO_MAKE_IS_REF(op1);
	Z_ADDREF_P(*op1);
	Z_TAINT_PP(op1, TAINT_ALL);
	TAINT_ARG_PUSH(*op1);

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}

	execute_data->opline++;
	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int my_send_var(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval **op1 = NULL;
	taint_free_op free_op1 = {0};
	zval *varptr;

	if ((opline->extended_value == ZEND_DO_FCALL_BY_NAME)
			&& ARG_SHOULD_BE_SENT_BY_REF(execute_data->fbc, TAINT_OP_LINENUM(opline->op2))) {
		return my_send_ref(ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
	}

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			op1 = TAINT_T(TAINT_OP1_VAR(opline)).var.ptr_ptr;
			break;
		case IS_CV:
			{
				zval **t = TAINT_CV_OF(TAINT_OP1_VAR(opline));
				if (t && *t) {
					op1 = t;
				} else if (EG(active_symbol_table)) {
					zend_compiled_variable *cv = &TAINT_CV_DEF_OF(TAINT_OP1_VAR(opline));
					if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len + 1, cv->hash_value, (void **)&t) == SUCCESS) {
						op1 = t;
					}
				}
			}
			break;
	}

	if (!op1 || *op1 == &EG(error_zval) || *op1 == &EG(uninitialized_zval) || IS_STRING != Z_TYPE_PP(op1) 
			|| !PZVAL_IS_REF(*op1) || Z_REFCOUNT_PP(op1) < 2 || !Z_STRLEN_PP(op1) || !PHP_TAINT_POSSIBLE((*op1))) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	MAKE_STD_ZVAL(varptr);
    *varptr = **op1;
	Z_SET_REFCOUNT_P(varptr, 0);
	zval_copy_ctor(varptr);
	Z_TAINT_P(varptr, TAINT_ALL);

	Z_ADDREF_P(varptr);
	TAINT_ARG_PUSH(varptr);

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}

	execute_data->opline++;
	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int my_qm_assign(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
    zend_op *opline = execute_data->opline;
	zval *op1 = NULL;
	taint_free_op free_op1 = {0};

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
			op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}

	TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var = *op1;

	if (!((zend_uintptr_t)free_op1.var & 1L)) {
		zval_copy_ctor(&TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var);
		if (op1 && IS_STRING == Z_TYPE_P(op1) && PHP_TAINT_POSSIBLE(op1)) {
			zval *result = &TAINT_T(TAINT_RESULT_VAR(opline)).tmp_var;
			Z_TAINT_P(result, op1->taint);
		}
	}

	switch (TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op1.var);
			break;
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int my_fetch_dim_func_arg(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *dim = NULL;
	zval **retval;
	zval *new_zval;
	long index;
	char *offset_key;
	int offset_key_length;
	taint_free_op free_op1 = {0};
	taint_free_op free_op2 = {0};

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
			op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}
	
	//this kind of webshell can not be mark: $test = '_POST';eval(${$test}['c']);, so mark it at main/main.c php_request_startup()
	if(IS_SIMULATED(op1) && Z_TYPE_P(op1) == IS_ARRAY) {
	//if(Z_TYPE_P(op1) == IS_ARRAY) {
		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				dim = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_VAR:
				dim = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_CV:
				dim = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
				break;
			case IS_CONST:
				dim = TAINT_OP2_CONSTANT_PTR(opline);
				break;
			case IS_UNUSED:
				dim = NULL;
				break;
			default:
				/* do nothing */
				break;
		}
		//if there is no such val, simulate one, and taint it
		if(dim->type == IS_LONG) {
			index = Z_LVAL_P(dim);
			if (zend_hash_index_find(Z_ARRVAL_P(op1), index, (void **) &retval) == FAILURE) {
				MAKE_STD_ZVAL(new_zval);
				ZVAL_STRING(new_zval, simstr, 1);
				Z_ADDREF_P(new_zval);
				Z_TAINT_P(new_zval, TAINT_ALL);
				zend_hash_index_update(Z_ARRVAL_P(op1), index, &new_zval, sizeof(zval *), (void **) &retval);
			}
		}
		else if(dim->type == IS_STRING) {
			offset_key = dim->value.str.val;
			offset_key_length = dim->value.str.len;
			if (zend_symtable_find(Z_ARRVAL_P(op1), offset_key, offset_key_length+1, (void **) &retval) == FAILURE) {
				MAKE_STD_ZVAL(new_zval);
				ZVAL_STRING(new_zval, simstr, 1);
				Z_ADDREF_P(new_zval);
				Z_TAINT_P(new_zval, TAINT_ALL);
				zend_symtable_update(Z_ARRVAL_P(op1), offset_key, offset_key_length+1, &new_zval, sizeof(zval *), (void **) &retval);
			}
		}
	}
/*
	switch (TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op1.var);
			break;
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}
	switch(TAINT_OP2_TYPE(opline)) {
	case IS_TMP_VAR:
		zval_ptr_dtor(&dim);
		break;
	case IS_VAR:
		if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
		break;
	case IS_CV:
	case IS_CONST:
	case IS_UNUSED:
	default:
		break;
	}
*/
	PZVAL_LOCK(op1); //avoid free op1 in ZEND_FETCH_DIM_R_SPEC_VAR_CONST_HANDLER
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

//because simulate GPCR arrays in main/main.c - php_request_startup, thif func is useless.
static int my_fetch_func_arg(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval *op1 = NULL;
	zval **retval;
	HashTable *ht;

	if(TAINT_OP1_TYPE(opline) == IS_CONST && opline->op2.u.EA.type == ZEND_FETCH_GLOBAL) {
		op1 = TAINT_OP1_CONSTANT_PTR(opline);

		if (op1 && op1 == &EG(error_zval)) {
			return ZEND_USER_OPCODE_DISPATCH;
		}
		//if fetch global GPC array, mark the return val, in order to simulate data then.
		if(Z_TYPE_P(op1) == IS_STRING && (strcmp(Z_STRVAL_P(op1),"_GET")==0 || strcmp(Z_STRVAL_P(op1),"_POST")==0 || strcmp(Z_STRVAL_P(op1),"_COOKIE")==0 || strcmp(Z_STRVAL_P(op1),"_REQUEST")==0)) {
			if (zend_hash_find(&EG(symbol_table), op1->value.str.val, op1->value.str.len+1, (void **) &retval) == FAILURE) {
					zend_error(E_NOTICE,"Undefined variable: %s", Z_STRVAL_P(op1));
					retval = &EG(uninitialized_zval_ptr);
					execute_data->opline++;
					return ZEND_USER_OPCODE_CONTINUE;
			}
			//mark GPC array
			Z_SIMULATE_PP(retval);
/*			
			if (!RETURN_VALUE_UNUSED(&opline->result)) {
				if (opline->extended_value & ZEND_FETCH_MAKE_REF) {
					SEPARATE_ZVAL_TO_MAKE_IS_REF(retval);
				}
				PZVAL_LOCK(*retval);
				TAINT_AI_SET_PTR(TAINT_T(TAINT_RESULT_VAR(opline)).var, *retval);
			}
			execute_data->opline++;
			return ZEND_USER_OPCODE_CONTINUE;
*/
		}
	}
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_fetch_dim_r(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval *op1 = NULL, *dim = NULL;
	zval **retval;
	zval *new_zval;
	long index;
	char *offset_key;
	int offset_key_length;
	taint_free_op free_op1 = {0};
	taint_free_op free_op2 = {0};

	switch(TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			op1 = php_taint_get_zval_ptr_tmp(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_VAR:
			op1 = php_taint_get_zval_ptr_var(TAINT_OP1_NODE_PTR(opline), execute_data->Ts, &free_op1 TSRMLS_CC);
			break;
		case IS_CV:
			op1 = php_taint_get_zval_ptr_cv(TAINT_OP1_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
			break;
		case IS_CONST:
			op1 = TAINT_OP1_CONSTANT_PTR(opline);
			break;
	}
	
	//this kind of webshell can not be mark: $test = '_POST';eval(${$test}['c']);, so mark it at main/main.c php_request_startup()
	if(IS_SIMULATED(op1) && Z_TYPE_P(op1) == IS_ARRAY) {
	//if(Z_TYPE_P(op1) == IS_ARRAY) {
		switch(TAINT_OP2_TYPE(opline)) {
			case IS_TMP_VAR:
				dim = php_taint_get_zval_ptr_tmp(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_VAR:
				dim = php_taint_get_zval_ptr_var(TAINT_OP2_NODE_PTR(opline), execute_data->Ts, &free_op2 TSRMLS_CC);
				break;
			case IS_CV:
				dim = php_taint_get_zval_ptr_cv(TAINT_OP2_NODE_PTR(opline), TAINT_GET_ZVAL_PTR_CV_2ND_ARG(BP_VAR_R) TSRMLS_CC);
				break;
			case IS_CONST:
				dim = TAINT_OP2_CONSTANT_PTR(opline);
				break;
			case IS_UNUSED:
				dim = NULL;
				break;
			default:
				/* do nothing */
				break;
		}
		//if there is no such val, simulate one
		if(dim->type == IS_LONG) {
			index = Z_LVAL_P(dim);
			if (zend_hash_index_find(Z_ARRVAL_P(op1), index, (void **) &retval) == FAILURE) {
				MAKE_STD_ZVAL(new_zval);
				ZVAL_STRING(new_zval, simstr, 1);
				Z_ADDREF_P(new_zval);
				Z_TAINT_P(new_zval, TAINT_ALL);
				zend_hash_index_update(Z_ARRVAL_P(op1), index, &new_zval, sizeof(zval *), (void **) &retval);
			}
		}
		else if(dim->type == IS_STRING) {
			offset_key = dim->value.str.val;
			offset_key_length = dim->value.str.len;
			if (zend_symtable_find(Z_ARRVAL_P(op1), offset_key, offset_key_length+1, (void **) &retval) == FAILURE) {
				MAKE_STD_ZVAL(new_zval);
				ZVAL_STRING(new_zval, simstr, 1);
				Z_ADDREF_P(new_zval);
				Z_TAINT_P(new_zval, TAINT_ALL);
				zend_symtable_update(Z_ARRVAL_P(op1), offset_key, offset_key_length+1, &new_zval, sizeof(zval *), (void **) &retval);
			}
		}
	}
/*
	switch (TAINT_OP1_TYPE(opline)) {
		case IS_TMP_VAR:
			zval_dtor(free_op1.var);
			break;
		case IS_VAR:
			if (free_op1.var) {
				zval_ptr_dtor(&free_op1.var);
			}
			break;
	}
	switch(TAINT_OP2_TYPE(opline)) {
	case IS_TMP_VAR:
		zval_ptr_dtor(&dim);
		break;
	case IS_VAR:
		if (free_op2.var) {zval_ptr_dtor(&free_op2.var);};
		break;
	case IS_CV:
	case IS_CONST:
	case IS_UNUSED:
	default:
		break;
	}
*/
	PZVAL_LOCK(op1); //avoid free op1 in ZEND_FETCH_DIM_R_SPEC_VAR_CONST_HANDLER
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

//because simulate GPCR arrays in main/main.c - php_request_startup, thif func is useless.
static int my_fetch_r(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval *op1 = NULL;
	zval **retval;

	if(TAINT_OP1_TYPE(opline) == IS_CONST && opline->op2.u.EA.type == ZEND_FETCH_GLOBAL) {
		op1 = TAINT_OP1_CONSTANT_PTR(opline);

		if (op1 && op1 == &EG(error_zval)) {
			return ZEND_USER_OPCODE_DISPATCH;
		}
		//if fetch global GPC array, mark the return val, in order to simulate data then.
		if(Z_TYPE_P(op1) == IS_STRING && (strcmp(Z_STRVAL_P(op1),"_GET")==0 || strcmp(Z_STRVAL_P(op1),"_POST")==0 || strcmp(Z_STRVAL_P(op1),"_COOKIE")==0 || strcmp(Z_STRVAL_P(op1),"_REQUEST")==0)) {
			if (zend_hash_find(&EG(symbol_table), op1->value.str.val, op1->value.str.len+1, (void **) &retval) == FAILURE) {
					zend_error(E_NOTICE,"Undefined variable: %s", Z_STRVAL_P(op1));
					retval = &EG(uninitialized_zval_ptr);
					execute_data->opline++;
					return ZEND_USER_OPCODE_CONTINUE;
			}

			//mark GPC array
			Z_SIMULATE_PP(retval);
			
			if (!RETURN_VALUE_UNUSED(&opline->result)) {
				if (opline->extended_value & ZEND_FETCH_MAKE_REF) {
					SEPARATE_ZVAL_TO_MAKE_IS_REF(retval);
				}
				PZVAL_LOCK(*retval);
				TAINT_AI_SET_PTR(TAINT_T(TAINT_RESULT_VAR(opline)).var, *retval);
			}
			execute_data->opline++;
			return ZEND_USER_OPCODE_CONTINUE;
		}
	}
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

//find old func and replace with the new func
static void php_taint_override_func(char *name, uint len, php_func handler, php_func *stash TSRMLS_DC) /* {{{ */ {
	zend_function *func;
	if (zend_hash_find(CG(function_table), name, len, (void **)&func) == SUCCESS) {
		if (stash) {
			*stash = func->internal_function.handler;
		}
		func->internal_function.handler = handler;
	}
} /* }}} */

/* {{{ proto string strval(mixed $value)
 */
PHP_FUNCTION(my_strval) {
	zval **arg;
	int tainted = 0;

	if (ZEND_NUM_ARGS() != 1 || zend_get_parameters_ex(1, &arg) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	if (Z_TYPE_PP(arg) == IS_STRING && PHP_TAINT_POSSIBLE((*arg))) {
		tainted = (*arg)->taint;
	}

    TAINT_O_FUNC(strval)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string sprintf(string $format, ...)
 */
PHP_FUNCTION(my_sprintf) {
	zval ***args;
	int i, argc, tainted = 0;

	argc = ZEND_NUM_ARGS();

	if (argc < 1) {
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	args = (zval ***)safe_emalloc(argc, sizeof(zval *), 0);
	if (zend_get_parameters_array_ex(argc, args) == FAILURE) {
		efree(args);
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	for (i=0; i<argc; i++) {
		if (args[i] && IS_STRING == Z_TYPE_PP(args[i]) && PHP_TAINT_POSSIBLE((*args[i]))) {
			tainted = (*args[i])->taint;
			break;
		}
	}
	efree(args);

	TAINT_O_FUNC(sprintf)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string vsprintf(string $format, ...)
 */
PHP_FUNCTION(my_vsprintf) {
	zval *format, *args;
	int argc, tainted = 0;

	argc = ZEND_NUM_ARGS();

	if (argc < 1) {
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	if (zend_parse_parameters(argc TSRMLS_CC, "za", &format, &args) == FAILURE) {
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	do {
		if (IS_STRING == Z_TYPE_P(format) &&  PHP_TAINT_POSSIBLE(format)) {
			tainted = format->taint;
			break;
		}

		if (IS_ARRAY == Z_TYPE_P(args)) {
			HashTable *ht = Z_ARRVAL_P(args);
			zval **ppzval;
			for(zend_hash_internal_pointer_reset(ht);
					zend_hash_has_more_elements(ht) == SUCCESS;
					zend_hash_move_forward(ht)) {
				if (zend_hash_get_current_data(ht, (void**)&ppzval) == FAILURE) {
					continue;
				}
				if (IS_STRING == Z_TYPE_PP(ppzval) && Z_STRLEN_PP(ppzval) && PHP_TAINT_POSSIBLE((*ppzval))) {
					tainted = (*ppzval)->taint;
					break;
				}
			}
			break;
		}
	} while (0);

	TAINT_O_FUNC(vsprintf)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto array explode(string $separator, string $str[, int $limit])
 */
PHP_FUNCTION(my_explode) {
	zval *separator, *str, *limit;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz|z", &separator, &str, &limit) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(explode)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_ARRAY == Z_TYPE_P(return_value) && zend_hash_num_elements(Z_ARRVAL_P(return_value))) {
		php_taint_mark_strings(return_value, tainted TSRMLS_CC);
	}
}
/* }}} */

 /* {{{ proto string implode(string $separator, array $args)
 */
PHP_FUNCTION(my_implode) {
	zval *op1, *op2;
	zval *target = NULL;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &op1, &op2) == FAILURE) {
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	if (IS_ARRAY == Z_TYPE_P(op1)) {
		target = op1;
	} else if(IS_ARRAY == Z_TYPE_P(op2)) {
		target = op2;
	}

	if (target) {
		HashTable *ht = Z_ARRVAL_P(target);
		zval **ppzval;
		for(zend_hash_internal_pointer_reset(ht);
				zend_hash_has_more_elements(ht) == SUCCESS;
				zend_hash_move_forward(ht)) {
			if (zend_hash_get_current_data(ht, (void**)&ppzval) == FAILURE) {
				continue;
			}
			if (IS_STRING == Z_TYPE_PP(ppzval) && Z_STRLEN_PP(ppzval) && PHP_TAINT_POSSIBLE((*ppzval))) {
				tainted = (*ppzval)->taint;
				break;
			}
		}
	}

	TAINT_O_FUNC(implode)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string trim(string $str)
 */
PHP_FUNCTION(my_trim)
{
	zval *str, *charlist;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z", &str, &charlist) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(trim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string rtrim(string $str)
 */
PHP_FUNCTION(my_rtrim)
{
	zval *str, *charlist;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z", &str, &charlist) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(rtrim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string ltrim(string $str)
 */
PHP_FUNCTION(my_ltrim)
{
	zval *str, *charlist;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z", &str, &charlist) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(ltrim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string str_replace(mixed $search, mixed $replace, mixed $subject [, int &$count])
 */
PHP_FUNCTION(my_str_replace)
{
	zval *str, *from, *len, *repl;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zzz|z", &str, &repl, &from, &len) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(repl) && PHP_TAINT_POSSIBLE(repl)) {
		tainted = repl->taint;
	} else if (IS_STRING == Z_TYPE_P(from) && PHP_TAINT_POSSIBLE(from)) {
		tainted = from->taint;
	}

	TAINT_O_FUNC(str_replace)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string str_pad(string $input, int $pad_length[, string $pad_string = " "[, int $pad_type = STR_PAD_RIGHT]])
 */
PHP_FUNCTION(my_str_pad)
{
	zval *input, *pad_length, *pad_string = NULL, *pad_type = NULL;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz|zz", &input, &pad_length, &pad_string, &pad_type) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(input) && PHP_TAINT_POSSIBLE(input)) {
		tainted = input->taint;
	} else if (pad_string && IS_STRING == Z_TYPE_P(pad_string) && PHP_TAINT_POSSIBLE(pad_string)) {
		tainted = pad_string->taint;
	}

	TAINT_O_FUNC(str_pad)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string strstr(string $haystack, mixed $needle[, bool $part = false])
 */
PHP_FUNCTION(my_strstr)
{
	zval *haystack, *needle, *part;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz|z", &haystack, &needle, &part) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(haystack) && PHP_TAINT_POSSIBLE(haystack)) {
		tainted = haystack->taint;
	}

	TAINT_O_FUNC(strstr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string substr(string $string, int $start[, int $length])
 */
PHP_FUNCTION(my_substr)
{
	zval *str;
	long start, length;
    int	tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zl|l", &str, &start, &length) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(substr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string strtolower(string $string)
 */
PHP_FUNCTION(my_strtolower)
{
	zval *str;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &str) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(strtolower)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string strtoupper(string $string)
 */
PHP_FUNCTION(my_strtoupper)
{
	zval *str;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &str) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(strtoupper)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string strtr(string $string, string $string, string $string)
 */
PHP_FUNCTION(my_strtr)
{
	int tainted = 0;
	zval **from;
	zval *str;
	char *to = NULL;
	int to_len = 0;
	int ac = ZEND_NUM_ARGS();
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zZ|s", &str, &from, &to, &to_len) == FAILURE) {
		return;
	}
	
	if (ac == 3 && IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(strtr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string str_rot13(string $string)
 */
PHP_FUNCTION(my_str_rot13)
{
	int tainted = 0;
	zval *str;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &str) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(str_rot13)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

/* {{{ proto string base64_decode(string $string)
 */
PHP_FUNCTION(my_base64_decode)
{
	int tainted = 0;
	zval *str;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &str) == FAILURE) {
		return;
	}
	
	if (IS_STRING == Z_TYPE_P(str) && PHP_TAINT_POSSIBLE(str)) {
		tainted = str->taint;
	}

	TAINT_O_FUNC(base64_decode)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	
	if (IS_TAINT_TAINTED(tainted) && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		Z_TAINT_P(return_value, tainted);
	}
}
/* }}} */

PHP_FUNCTION(my_ob_start)
{
	char *handler_name;
	zval *output_handler=NULL;
	long chunk_size=0;
	zend_bool erase=1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|zlb", &output_handler, &chunk_size, &erase) == FAILURE) {
		return;
	}
	if (output_handler && output_handler->type == IS_STRING) {
		handler_name = Z_STRVAL_P(output_handler);
		if (strstr(handler_name, "eval") || strstr(handler_name, "assert") || strstr(handler_name, "preg_replace")) {
			error_output("function.ob_start" TSRMLS_CC, EG(current_execute_data)->opline->lineno, "ob_start code might be tainted");
		}
	}
	
	if (chunk_size < 0)
		chunk_size = 0;

	if (php_start_ob_buffer(output_handler, chunk_size, erase TSRMLS_CC)==FAILURE) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}

PHP_FUNCTION(my_assert)
{
	zval **assertion;
	int description_len = 0;
	char *description = NULL;


//	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Z", &assertion) == FAILURE) {
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Z|s", &assertion, &description, &description_len) == FAILURE) {
		RETURN_FALSE;
	}

	if (Z_TYPE_PP(assertion) == IS_STRING && IS_TAINT_TAINTED((*assertion)->taint)) {
	  	error_output("function.assert" TSRMLS_CC, EG(current_execute_data)->opline->lineno, "assert code might be tainted");
	}
	
	TAINT_O_FUNC(assert)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static zend_op_array *my_compile_string(zval *source_string, char *filename TSRMLS_DC)
{
  //php_printf("in my_compile_string:%s filename:%s\n", Z_STRVAL_P(source_string), filename);
  if (PHP_TAINT_POSSIBLE(source_string)) {
	  if (strstr(filename, "assert code")) {
	  	error_output("function.assert" TSRMLS_CC, EG(current_execute_data)->opline->lineno, "assert code might be tainted");
	  }
	  else if (strstr(filename, "regexp code")) {
	  	error_output("function.preg_replace/e" TSRMLS_CC, EG(current_execute_data)->opline->lineno, "preg_replace/e might be tainted");
	  }
	  else if (strstr(filename, "mbregex replace")) {
	  	error_output("function.mb_ereg_replace/e" TSRMLS_CC, EG(current_execute_data)->opline->lineno, "mb_ereg_replace/e might be tainted");
	  }
	}
  return old_compile_string(source_string, filename TSRMLS_CC);
}

//hook php kenerl functions
void hook_php()
{
	//hook string functions
	char f_join[]        = "join";
	char f_trim[]        = "trim";
	char f_split[]       = "split";
	char f_rtrim[]       = "rtrim";
	char f_ltrim[]       = "ltrim";
	char f_strval[]      = "strval";
	char f_strstr[]      = "strstr";
	char f_substr[]      = "substr";
	char f_sprintf[]     = "sprintf";
	char f_explode[]     = "explode";
	char f_implode[]     = "implode";
	char f_str_pad[]     = "str_pad";
	char f_vsprintf[]    = "vsprintf";
	char f_str_replace[] = "str_replace";
	char f_strtolower[] = "strtolower";
	char f_strtoupper[] = "strtoupper";
	char f_strtr[] 			= "strtr";
	char f_str_rot13[] 	= "str_rot13";
	char f_base64_decode[] = "base64_decode";
	char f_ob_start[]		= "ob_start";
	char f_assert[]			= "assert";

	php_taint_override_func(f_strval, sizeof(f_strval), PHP_FN(my_strval), &TAINT_O_FUNC(strval) TSRMLS_CC);
	php_taint_override_func(f_sprintf, sizeof(f_sprintf), PHP_FN(my_sprintf), &TAINT_O_FUNC(sprintf) TSRMLS_CC);
	php_taint_override_func(f_vsprintf, sizeof(f_vsprintf), PHP_FN(my_vsprintf), &TAINT_O_FUNC(vsprintf) TSRMLS_CC);
	php_taint_override_func(f_explode, sizeof(f_explode), PHP_FN(my_explode), &TAINT_O_FUNC(explode) TSRMLS_CC);
	php_taint_override_func(f_split, sizeof(f_split), PHP_FN(my_explode), NULL TSRMLS_CC);
	php_taint_override_func(f_implode, sizeof(f_implode), PHP_FN(my_implode), &TAINT_O_FUNC(implode) TSRMLS_CC);
	php_taint_override_func(f_join, sizeof(f_join), PHP_FN(my_implode), NULL TSRMLS_CC);
	php_taint_override_func(f_trim, sizeof(f_trim), PHP_FN(my_trim), &TAINT_O_FUNC(trim) TSRMLS_CC);
	php_taint_override_func(f_rtrim, sizeof(f_rtrim), PHP_FN(my_rtrim), &TAINT_O_FUNC(rtrim) TSRMLS_CC);
	php_taint_override_func(f_ltrim, sizeof(f_ltrim), PHP_FN(my_ltrim), &TAINT_O_FUNC(ltrim) TSRMLS_CC);
	php_taint_override_func(f_str_replace, sizeof(f_str_replace), PHP_FN(my_str_replace), &TAINT_O_FUNC(str_replace) TSRMLS_CC);
	php_taint_override_func(f_str_pad, sizeof(f_str_pad), PHP_FN(my_str_pad), &TAINT_O_FUNC(str_pad) TSRMLS_CC);
	php_taint_override_func(f_strstr, sizeof(f_strstr), PHP_FN(my_strstr), &TAINT_O_FUNC(strstr) TSRMLS_CC);
	php_taint_override_func(f_strtolower, sizeof(f_strtolower), PHP_FN(my_strtolower), &TAINT_O_FUNC(strtolower) TSRMLS_CC);
	php_taint_override_func(f_strtoupper, sizeof(f_strtoupper), PHP_FN(my_strtoupper), &TAINT_O_FUNC(strtoupper) TSRMLS_CC);
	php_taint_override_func(f_strtr, sizeof(f_strtr), PHP_FN(my_strtr), &TAINT_O_FUNC(strtr) TSRMLS_CC);
	php_taint_override_func(f_str_rot13, sizeof(f_str_rot13), PHP_FN(my_str_rot13), &TAINT_O_FUNC(str_rot13) TSRMLS_CC);
	php_taint_override_func(f_base64_decode, sizeof(f_base64_decode), PHP_FN(my_base64_decode), &TAINT_O_FUNC(base64_decode) TSRMLS_CC);
	php_taint_override_func(f_substr, sizeof(f_substr), PHP_FN(my_substr), &TAINT_O_FUNC(substr) TSRMLS_CC);
	php_taint_override_func(f_ob_start, sizeof(f_ob_start), PHP_FN(my_ob_start), &TAINT_O_FUNC(ob_start) TSRMLS_CC);
	php_taint_override_func(f_assert, sizeof(f_assert), PHP_FN(my_assert), &TAINT_O_FUNC(assert) TSRMLS_CC);

	//hook zend_compile_string to detect assert	
	old_compile_string = zend_compile_string;
 	zend_compile_string = my_compile_string; 

	//set opcode handler to hook kernel functions
	zend_set_user_opcode_handler(ZEND_INCLUDE_OR_EVAL, my_include_or_eval);		//eval/include function
	zend_set_user_opcode_handler(ZEND_DO_FCALL, my_do_fcall);									//call some function,like system/passthru/exec/proc_open/shell_exec/fopen
	zend_set_user_opcode_handler(ZEND_DO_FCALL_BY_NAME, my_do_fcall_by_name);	//call function by name,eg,system("dir");
	zend_set_user_opcode_handler(ZEND_INIT_FCALL_BY_NAME, my_init_fcall_by_name);	//call function by var,eg,$a="system";$a("dir");
	//string operation kernel functions 
	zend_set_user_opcode_handler(ZEND_CONCAT, my_concat);											//string connect,eg,$a="xx"."oo"
	zend_set_user_opcode_handler(ZEND_ASSIGN_CONCAT, my_assign_concat);				//string connect assign,eg,$a.="xx"
	zend_set_user_opcode_handler(ZEND_ADD_CHAR, my_add_char);									//add char to string,eg,$a="xx $b"
	zend_set_user_opcode_handler(ZEND_ADD_STRING, my_add_string);							//add string to string,eg,$a="xx $b"
	zend_set_user_opcode_handler(ZEND_ADD_VAR, my_add_var);										//Inserts a variable into a string,eg,echo "$b"
	zend_set_user_opcode_handler(ZEND_ASSIGN_REF, my_assign_ref);							//assign ref,eg,$a=&$b;
	zend_set_user_opcode_handler(ZEND_ASSIGN, my_assign);											//assign,eg,$a=$b
	zend_set_user_opcode_handler(ZEND_SEND_VAR, my_send_var);									//send var to a function
  zend_set_user_opcode_handler(ZEND_SEND_REF, my_send_ref);									//send var ref to a function
	zend_set_user_opcode_handler(ZEND_QM_ASSIGN, my_qm_assign);								//Assigns a value to the result of a ternary operator expression,eg,$x=(3>4)?1:2;
	if(simstr) {
		//zend_set_user_opcode_handler(ZEND_FETCH_R, my_fetch_r);										//fetch global GPC data
		zend_set_user_opcode_handler(ZEND_FETCH_DIM_R, my_fetch_dim_r);						//fetch global GPC data
		//zend_set_user_opcode_handler(ZEND_FETCH_FUNC_ARG, my_fetch_func_arg);										//fetch global GPC data
		zend_set_user_opcode_handler(ZEND_FETCH_DIM_FUNC_ARG, my_fetch_dim_func_arg);	//fetch global GPC data
	}
	
	return;
}

char *strtoupper(char *str) {
    int i = 0;
    char *p = (char*) malloc((strlen(str) + 1) * sizeof(char));
    for(; str[i] != '\0'; ++i){
        if((str[i] >= 'a') && (str[i] <= 'z'))
            p[i] = str[i] + 'A' - 'a';
        else
            p[i] = str[i];
    }
    p[i] = '\0';
    return p;
}
//decode the POST data
//input:	\xAA\xBB\xCC\xDD
//output:	AABBCCDD
int decodestr(char *data)
{
	int i, out_index = 0;
	int input_len = strlen(data);
	char chr[4];
	char *out;
	char *ptr = strtoupper(data);
	out = (char *)emalloc(input_len);
	
	for (i=0; i<input_len; i+=4)
	{
		ptr+=2;
		sscanf(ptr, "%02X", &chr[0]);
		out[out_index++] = chr[0];
		ptr+=2;
	}
	memcpy(data, out, out_index);
	data[out_index] = 0;
	efree(out);
	return out_index;
}

/* {{{ main
 */
int main(int argc, char *argv[])
{
	int free_query_string = 0;
	int exit_status = SUCCESS;
	int c;
	zend_file_handle file_handle;
	int decodeinput = 0;

	/* temporary locals */
	int no_headers = 0;
	int orig_optind = php_optind;
	char *orig_optarg = php_optarg;
	/* end of temporary locals */

#ifdef ZTS
	void ***tsrm_ls;
#endif

	pvm_request request;

	if(argc < 2)
	{
		no_headers = 1;
		php_output_startup();
		php_output_activate(TSRMLS_C);
		SG(headers_sent) = 1;
		php_cgi_usage(argv[0]);
		php_end_ob_buffers(1 TSRMLS_CC);
		exit_status = 0;
		return 1;
	}

#ifdef ZTS
	tsrm_startup(1, 1, 0, NULL);
	tsrm_ls = ts_resource(0);
#endif

	sapi_startup(&cgi_sapi_module);
	cgi_sapi_module.php_ini_path_override = NULL;

#ifdef PHP_WIN32
	_fmode = _O_BINARY; /* sets default for file streams to binary */
	setmode(_fileno(stdin),  O_BINARY);	/* make the stdio mode be binary */
	setmode(_fileno(stdout), O_BINARY);	/* make the stdio mode be binary */
	setmode(_fileno(stderr), O_BINARY);	/* make the stdio mode be binary */
#endif

	php_optind = orig_optind;
	php_optarg = orig_optarg;

#ifdef ZTS
	SG(request_info).path_translated = NULL;
#endif

	cgi_sapi_module.executable_location = argv[0];
	cgi_sapi_module.additional_functions = additional_functions;

	/* startup after we get the above ini override se we get things right */
	if (cgi_sapi_module.startup(&cgi_sapi_module) == FAILURE) {
#ifdef ZTS
		tsrm_shutdown();
#endif
		return FAILURE;
	}

	zend_first_try {
		while ((c = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 1, 2)) != -1) {
			switch (c) {
				case 'h':
				case '?':
					no_headers = 1;
					php_output_startup();
					php_output_activate(TSRMLS_C);
					SG(headers_sent) = 1;
					php_cgi_usage(argv[0]);
					php_end_ob_buffers(1 TSRMLS_CC);
					exit_status = 0;
					goto out;
			}
		}
		php_optind = orig_optind;
		php_optarg = orig_optarg;

		SG(server_context) = (void *) &request;
		init_request_info(TSRMLS_C);
		CG(interactive) = 0;

		while ((c = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 0, 2)) != -1) {
					switch (c) {
						case 'f': /* parse file */
							if (script_file) {
								efree(script_file);
							}
							script_file = estrdup(php_optarg);
							no_headers = 1;
							break;
							
						case 'l':
							logfile = estrdup(php_optarg);
							break;

						case 'd':
							decodeinput = 1;
							break;

						case 'g':
							getstr = estrdup(php_optarg);
							break;

						case 'p':
							poststr = estrdup(php_optarg);
							break;

						case 'i':
							poststr = get_file_content(estrdup(php_optarg));
							break;

						case 'k':
							cookiestr = estrdup(php_optarg);
							break;

						case 't':
							methodstr = estrdup(php_optarg);
							break;

						case 's':
							simstr = estrdup(php_optarg);
							break;

						default:
							break;
					}
		}
		//do not dispay html errors
		PG(html_errors) = 0;
		//POST data is encoded, we should decode at first
		if (decodeinput == 1) {
			//getstrlen = decodestr(getstr);
			if (poststr) {
				poststrlen = decodestr(poststr);
			}
			//cookiestrlen = decodestr(cookiestr);
		}
		else {
			//getstrlen = strlen(getstr);
			if (poststr) {
				poststrlen = strlen(poststr);
			}
			//cookiestrlen = strlen(cookiestr);
		}
		//original input data will be modified by SAPI,so we make a backup
		org_getstr = getstr;//(char *)emalloc(strlen(getstr)+1);
		org_poststr = (char *)emalloc(poststrlen+1);
		org_cookiestr = cookiestr;//(char *)emalloc(strlen(cookiestr)+1);
		//strcpy(org_getstr, getstr);
		if (poststr) {
			strcpy(org_poststr, poststr);
		}
		else {
			efree(org_poststr);
			org_poststr = NULL;
		}
		printf(poststr);
		//strcpy(org_cookiestr, cookiestr);
		
		if (script_file) {
					/* override path_translated if -f on command line */
					STR_FREE(SG(request_info).path_translated);
					SG(request_info).path_translated = script_file;
					/* before registering argv to module exchange the *new* argv[0] */
					/* we can achieve this without allocating more memory */
					SG(request_info).argc = argc - (php_optind - 1);
					SG(request_info).argv = &argv[php_optind - 1];
					SG(request_info).argv[0] = script_file;
		} else {
					no_headers = 1;
					SG(headers_sent) = 1;
					printf("need -f opt\n");
					php_end_ob_buffers(1 TSRMLS_CC);
					exit_status = 0;
					goto out;
		}

		if (no_headers) {
					SG(headers_sent) = 1;
					SG(request_info).no_headers = 1;
		}

		//set request input data
		SG(request_info).query_string = getstr;
		SG(request_info).request_method = methodstr;
		if (poststr) {
			SG(request_info).post_data = poststr;
			SG(request_info).post_data_length = poststrlen;
		}
		SG(request_info).content_type = typestr;
		SG(request_info).content_length = strlen(typestr);
		SG(request_info).post_entry = NULL;

		_sapi_cgibin_putenv("REQUEST_METHOD", methodstr TSRMLS_CC);
				
		free_query_string = 0;
				
		//hook PHP kernel functions
		hook_php();

		/*
					we never take stdin if we're (f)cgi, always
					rely on the web server giving us the info
					we need in the environment.
		*/
		if (SG(request_info).path_translated) {
					file_handle.type = ZEND_HANDLE_FILENAME;
					file_handle.filename = SG(request_info).path_translated;
					file_handle.handle.fp = NULL;
		} else {
					file_handle.filename = "-";
					file_handle.type = ZEND_HANDLE_FP;
					file_handle.handle.fp = stdin;
		}
	
		file_handle.opened_path = NULL;
		file_handle.free_filename = 0;
	
		/* request startup only after we've done all we can to
		 * get path_translated 
		 * POST DATA will be urldecoded here, original data will be modified */
		if (php_request_startup(TSRMLS_C) == FAILURE) {
					SG(server_context) = NULL;
					php_module_shutdown(TSRMLS_C);
					return FAILURE;
		}
		if (no_headers) {
					SG(headers_sent) = 1;
					SG(request_info).no_headers = 1;
		}
	
		if (php_fopen_primary_script(&file_handle TSRMLS_CC) == FAILURE) {
						zend_try {
							if (errno == EACCES) {
								SG(sapi_headers).http_response_code = 403;
								PUTS("Access denied.\n");
							} else {
								SG(sapi_headers).http_response_code = 404;
								PUTS("No input file specified.\n");
							}
						} zend_catch {
						} zend_end_try();
	
						STR_FREE(SG(request_info).path_translated);
	
						if (free_query_string && SG(request_info).query_string) {
							free(SG(request_info).query_string);
							SG(request_info).query_string = NULL;
						}
	
						php_request_shutdown((void *) 0);
						SG(server_context) = NULL;
						php_module_shutdown(TSRMLS_C);
						sapi_shutdown();
#ifdef ZTS
						tsrm_shutdown();
#endif
						return FAILURE;
		}
	
		//execute the main php script
		php_execute_script(&file_handle TSRMLS_CC);

		if (cgi_sapi_module.php_ini_path_override) {
			free(cgi_sapi_module.php_ini_path_override);
		}
		if (cgi_sapi_module.ini_entries) {
			free(cgi_sapi_module.ini_entries);
		}
	} zend_catch {
		exit_status = 255;
	} zend_end_try();

out:
	if (org_poststr) {
		efree(org_poststr);
	}
#ifndef PHP_WIN32
parent_out:
#endif

	SG(server_context) = NULL;
	php_module_shutdown(TSRMLS_C);
	sapi_shutdown();

#ifdef ZTS
	tsrm_shutdown();
#endif

#if defined(PHP_WIN32) && ZEND_DEBUG && 0
	_CrtDumpMemoryLeaks();
#endif

	return exit_status;
}
/* }}} */