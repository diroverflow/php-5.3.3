/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2010 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id: header 297205 2010-03-30 21:09:07Z johannes $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_pwatch.h"
#include "SAPI.h"

//hook zend_compile_string
static zend_op_array* (*old_compile_string)(zval *source_string, char *filename TSRMLS_DC);
static zend_op_array* my_compile_string(zval *source_string, char *filename TSRMLS_DC);

//pvm execute file path
static char pvm_path[] = "/tmp/pvm";//"/root/php-5.3.3/sapi/pvm/pvm";
static char log_file[] = "/tmp/pvm.log";

/* If you declare any globals in php_pwatch.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(pwatch)
*/

/* True global resources - no need for thread safety here */
static int le_pwatch;

/* {{{ pwatch_functions[]
 *
 * Every user visible function must have an entry in pwatch_functions[].
 */
const zend_function_entry pwatch_functions[] = {
	PHP_FE(confirm_pwatch_compiled,	NULL)		/* For testing, remove later. */
	{NULL, NULL, NULL}	/* Must be the last line in pwatch_functions[] */
};
/* }}} */

/* {{{ pwatch_module_entry
 */
zend_module_entry pwatch_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"pwatch",
	pwatch_functions,
	PHP_MINIT(pwatch),
	PHP_MSHUTDOWN(pwatch),
	PHP_RINIT(pwatch),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(pwatch),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(pwatch),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1", /* Replace with version number for your extension */
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PWATCH
ZEND_GET_MODULE(pwatch)
#endif

char *get_global_vars(zval *symbol_table) {
	char *ret_val = NULL;
	int ret_len = 0;
	int key_ret = HASH_KEY_NON_EXISTANT;
	char *key;
  int key_len;
  long index;
  char tmp[32] = {0};
  int have_key = 0;
	zval **ppzval;
	HashTable *ht = Z_ARRVAL_P(symbol_table);
	HashPosition pos = {0};
	
	ret_val = (char *)emalloc(1);
	ret_val[0] = 0;

	for(zend_hash_internal_pointer_reset_ex(ht, &pos);
			zend_hash_has_more_elements_ex(ht, &pos) == SUCCESS;
			zend_hash_move_forward_ex(ht, &pos)) {
		if (ret_len > 0) {
			ret_len++;
    	ret_val = (char *)erealloc(ret_val, ret_len);
    	strcat(ret_val, "&");
		}
		key_ret = zend_hash_get_current_key_ex(ht, &key, &key_len, &index, 0, &pos);
		if (key_ret == HASH_KEY_IS_STRING) {
      //php_printf("str key:%s\n",key);
      ret_len += key_len;
      ret_val = (char *)erealloc(ret_val, ret_len);
      strcat(ret_val, key);
      have_key = 1;
    }
    else if (key_ret == HASH_KEY_IS_LONG) {
      sprintf(tmp, "%d", index);
      //php_printf("num key:%s\n",tmp);
      key_len = strlen(tmp);
      ret_len += key_len;
      ret_val = (char *)erealloc(ret_val, ret_len);
      strcat(ret_val, tmp);
      have_key = 1;
		}
		else {
			have_key = 0;
		}    	
		if (zend_hash_get_current_data_ex(ht, (void**)&ppzval, &pos) == FAILURE) {
			continue;
		}
    if (IS_STRING == Z_TYPE_PP(ppzval) && Z_STRLEN_PP(ppzval) > 0) {
			//php_printf("val:%s\n",Z_STRVAL_PP(ppzval));
			if (have_key == 1) {
	      ret_len++;
	      ret_val = (char *)erealloc(ret_val, ret_len);
	      strcat(ret_val, "=");
	    }
      ret_len += (Z_STRLEN_PP(ppzval)+1);
      ret_val = (char *)erealloc(ret_val, ret_len);
      strcat(ret_val, Z_STRVAL_PP(ppzval));
		}
	}
	//php_printf("ret_val:%s, ret_len:%d\n", ret_val, ret_len);
	return ret_val;
} /* }}} */

char *encode_data(char *input) {
	char tmp[8] = {0};
	int i = 0;
	int input_len = strlen(input);
	char *buf = (char *)emalloc(input_len*5+1);
	buf[0] = 0;
	for (i=0; i<input_len; i++) {
		sprintf(tmp, "\\\\x%.02X", input[i]);
		strcat(buf, tmp);
	}
	return buf;
}

void sendto_pvm() {
	char *pvm_cmd;
	int getlen, postlen, cookielen, methodlen, scriptlen;

	char methodstr[] = "POST";
	char *scriptstr = zend_get_executed_filename(TSRMLS_C);
	if (strcmp(scriptstr, "[no active file]")==0 || memcmp(scriptstr, "Unknown(", 8)==0)
	{
		php_printf("can not get script file name.\n");
		return;
	}
	char *getstr = get_global_vars(PG(http_globals)[TRACK_VARS_GET]);
	char *poststr = get_global_vars(PG(http_globals)[TRACK_VARS_POST]);
	char *cookiestr = get_global_vars(PG(http_globals)[TRACK_VARS_COOKIE]);
	char *poststr_encode = encode_data(poststr);

//	php_printf(getstr);
//	php_printf(poststr);
//	php_printf(cookiestr);
//	php_printf(scriptstr);
	
	getlen = strlen(getstr);
	postlen = strlen(poststr_encode);
	cookielen = strlen(cookiestr);
	methodlen = strlen(methodstr);
	scriptlen = strlen(scriptstr);
	pvm_cmd = (char *)emalloc(scriptlen+getlen+postlen+cookielen+methodlen+strlen(log_file)+strlen(pvm_path)+64);
	strcpy(pvm_cmd, pvm_path);
	strcat(pvm_cmd, " ");
	if (getlen > 0) {
		strcat(pvm_cmd, "-g \"");
		strcat(pvm_cmd, getstr);
		strcat(pvm_cmd, "\" ");
	}
	if (postlen > 0) {
		strcat(pvm_cmd, "-d -p \"");
		strcat(pvm_cmd, poststr_encode);
		strcat(pvm_cmd, "\" ");
	}
	if (cookielen > 0) {
		strcat(pvm_cmd, "-k \"");
		strcat(pvm_cmd, cookiestr);
		strcat(pvm_cmd, "\" ");
	}
	strcat(pvm_cmd, "-t ");
	strcat(pvm_cmd, methodstr);
	strcat(pvm_cmd, " ");
	strcat(pvm_cmd, "-l ");
	strcat(pvm_cmd, log_file);
	strcat(pvm_cmd, " ");
	strcat(pvm_cmd, "-f ");
	strcat(pvm_cmd, scriptstr);
	//sprintf(pvm_cmd, "%s -g %s -p %s -k %s -t %s -f %s", pvm_path, SG(request_info).query_string, SG(request_info).post_data, SG(request_info).cookie_data, SG(request_info).request_method, SG(request_info).path_translated);
	//php_printf(pvm_cmd);
	getlen = system(pvm_cmd);
	//php_printf("system:%d.\n", getlen);
	if (getstr) efree(getstr);
	if (poststr) efree(poststr);
	if (cookiestr) efree(cookiestr);
	if (poststr_encode) efree(poststr_encode);
	if (pvm_cmd) efree(pvm_cmd);
}

static int my_do_fcall(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
  zend_op *opline = execute_data->opline;
	zval *fname = TAINT_OP1_CONSTANT_PTR(opline);
	char *funcname = Z_STRVAL_P(fname);
	int len = strlen(funcname);

	//php_printf("fname:%s\n", Z_STRVAL_P(fname));
	if (fname) {
		if (strncmp("passthru", funcname, len) == 0
					|| strncmp("system", funcname, len) == 0
					|| strncmp("exec", funcname, len) == 0
					|| strncmp("shell_exec", funcname, len) == 0
					|| strncmp("proc_open", funcname, len) == 0 ) {
						//php_printf("in exec funcs\n");
						sendto_pvm();
			}
	}
	
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int my_include_or_eval(ZEND_OPCODE_HANDLER_ARGS) /* {{{ */ {
	//php_printf("in eval funcs\n");
	sendto_pvm();
	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static void hook_register_handlers(TSRMLS_D) /* {{{ */ {
	zend_set_user_opcode_handler(ZEND_INCLUDE_OR_EVAL, my_include_or_eval);
	zend_set_user_opcode_handler(ZEND_DO_FCALL, my_do_fcall);
} /* }}} */

static zend_op_array *my_compile_string(zval *source_string, char *filename TSRMLS_DC)
{
  /* ¹ýÂËassertº¯Êý */
  if(strstr(filename, "assert code") || strstr(filename, "regexp code") || strstr(filename, "mbregex replace")) {
  	//php_printf("in assert funcs\n");
  	//php_printf(filename);
  	sendto_pvm();
  }
  return old_compile_string(source_string, filename TSRMLS_CC);
}
    
int hook_execute()
{ 			
	old_compile_string = zend_compile_string;
 	zend_compile_string = my_compile_string; 
 	return 1;
}
    
int unhook_execute()
{	
	zend_compile_string = old_compile_string;    
	return 1;
}
/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pwatch.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_pwatch_globals, pwatch_globals)
    STD_PHP_INI_ENTRY("pwatch.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_pwatch_globals, pwatch_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_pwatch_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_pwatch_init_globals(zend_pwatch_globals *pwatch_globals)
{
	pwatch_globals->global_value = 0;
	pwatch_globals->global_string = NULL;
}
*/
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(pwatch)
{
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/
	hook_register_handlers(TSRMLS_C);
	hook_execute();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pwatch)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	unhook_execute();
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(pwatch)
{
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(pwatch)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pwatch)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pwatch support", "enabled");
	php_info_print_table_row(2, "Version", "0.1");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */


/* Remove the following function when you have succesfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_pwatch_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(confirm_pwatch_compiled)
{
	char *arg = NULL;
	int arg_len, len;
	char *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	len = spprintf(&strg, 0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "pwatch", arg);
	RETURN_STRINGL(strg, len, 0);
}
/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and 
   unfold functions in source code. See the corresponding marks just before 
   function definition, where the functions purpose is also documented. Please 
   follow this convention for the convenience of others editing your code.
*/


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
