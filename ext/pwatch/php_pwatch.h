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

#ifndef PHP_PWATCH_H
#define PHP_PWATCH_H

extern zend_module_entry pwatch_module_entry;
#define phpext_pwatch_ptr &pwatch_module_entry

#ifdef PHP_WIN32
#	define PHP_PWATCH_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_PWATCH_API __attribute__ ((visibility("default")))
#else
#	define PHP_PWATCH_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(pwatch);
PHP_MSHUTDOWN_FUNCTION(pwatch);
PHP_RINIT_FUNCTION(pwatch);
PHP_RSHUTDOWN_FUNCTION(pwatch);
PHP_MINFO_FUNCTION(pwatch);

PHP_FUNCTION(confirm_pwatch_compiled);	/* For testing, remove later. */

/* 
  	Declare any global variables you may need between the BEGIN
	and END macros here:     

ZEND_BEGIN_MODULE_GLOBALS(pwatch)
	long  global_value;
	char *global_string;
ZEND_END_MODULE_GLOBALS(pwatch)
*/

/* In every utility function you add that needs to use variables 
   in php_pwatch_globals, call TSRMLS_FETCH(); after declaring other 
   variables used by that function, or better yet, pass in TSRMLS_CC
   after the last function argument and declare your utility function
   with TSRMLS_DC after the last declared argument.  Always refer to
   the globals in your function as PWATCH_G(variable).  You are 
   encouraged to rename these macros something shorter, see
   examples in any other php module directory.
*/
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 4) 
#  define TAINT_OP1_TYPE(n)         ((n)->op1.op_type)
#  define TAINT_OP2_TYPE(n)         ((n)->op2.op_type)
#  define TAINT_OP1_NODE_PTR(n)     (&(n)->op1)
#  define TAINT_OP2_NODE_PTR(n)     (&(n)->op2)
#  define TAINT_OP1_VAR(n)          ((n)->op1.u.var)
#  define TAINT_OP2_VAR(n)          ((n)->op2.u.var)
#  define TAINT_RESULT_VAR(n)       ((n)->result.u.var)
#  define TAINT_OP1_CONSTANT_PTR(n) (&(n)->op1.u.constant)
#  define TAINT_OP2_CONSTANT_PTR(n) (&(n)->op2.u.constant)
#  define TAINT_GET_ZVAL_PTR_CV_2ND_ARG(t) (execute_data->Ts)
#  define TAINT_RETURN_VALUE_USED(n) (!((&(n)->result)->u.EA.type & EXT_TYPE_UNUSED))
#  define TAINT_OP_LINENUM(n)       ((n).u.opline_num)
#  define TAINT_AI_SET_PTR(ai, val)		\
	(ai).ptr = (val);					\
	(ai).ptr_ptr = &((ai).ptr);
#else
#  define TAINT_OP1_TYPE(n)         ((n)->op1_type)
#  define TAINT_OP2_TYPE(n)         ((n)->op2_type)
#  define TAINT_OP1_NODE_PTR(n)     ((n)->op1.var)
#  define TAINT_OP2_NODE_PTR(n)     ((n)->op2.var)
#  define TAINT_OP1_VAR(n)          ((n)->op1.var)
#  define TAINT_OP2_VAR(n)          ((n)->op2.var)
#  define TAINT_RESULT_VAR(n)       ((n)->result.var)
#  define TAINT_OP1_CONSTANT_PTR(n) ((n)->op1.zv)
#  define TAINT_OP2_CONSTANT_PTR(n) ((n)->op2.zv)
#  define TAINT_GET_ZVAL_PTR_CV_2ND_ARG(t) (t)
#  define TAINT_RETURN_VALUE_USED(n) (!((n)->result_type & EXT_TYPE_UNUSED))
#  define TAINT_OP_LINENUM(n)       ((n).opline_num)
#  define TAINT_AI_SET_PTR(t, val) do {		\
		temp_variable *__t = (t);			\
		__t->var.ptr = (val);				\
		__t->var.ptr_ptr = &__t->var.ptr;	\
	} while (0)
#endif

#ifdef ZTS
#define PWATCH_G(v) TSRMG(pwatch_globals_id, zend_pwatch_globals *, v)
#else
#define PWATCH_G(v) (pwatch_globals.v)
#endif

#endif	/* PHP_PWATCH_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
