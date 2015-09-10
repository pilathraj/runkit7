/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group, (c) 2008-2012 Dmitry Zenovich |
  +----------------------------------------------------------------------+
  | This source file is subject to the new BSD license,                  |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.opensource.org/licenses/BSD-3-Clause                      |
  | If you did not receive a copy of the license and are unable to       |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  | Modified by Dmitry Zenovich <dzenovich@gmail.com>                    |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "php_runkit.h"

#ifdef PHP_RUNKIT_MANIPULATION_CONSTANTS
// Copied from zend_compile.c, RC2
/* {{{ php_runkit_fetch_const
 */
static int php_runkit_fetch_const(zend_string *cname_zs, zend_constant **constant, char **found_cname TSRMLS_DC)
{
	char* cname = ZSTR_VAL(cname_zs);
	int cname_len = ZSTR_LEN(cname_zs);
	char *lcase = NULL;
	char *old_cname = cname;
	int constant_name_len = cname_len;

	char *separator;

	if (cname_len > 0 && cname[0] == '\\') {
		++cname;
		--cname_len;
	}

	separator = (char *) zend_memrchr(cname, '\\', cname_len);
	if (separator) {
		int nsname_len = separator - cname;
		char *constant_name = separator + 1;
		constant_name_len = cname_len - nsname_len - 1;

		lcase = emalloc(nsname_len + 1 + constant_name_len + 1);
		memcpy(lcase, cname, nsname_len + 1);
		memcpy(lcase + nsname_len + 1, constant_name, constant_name_len + 1);
		zend_str_tolower(lcase, nsname_len);
		cname = lcase;
	}
	// TODO: Figure out how to extract from zend_constants
	if ((*constant = zend_hash_str_find_ptr(EG(zend_constants), cname, cname_len)) == NULL) {
		if (!lcase) {
			lcase = estrndup(cname, cname_len);
			zend_str_tolower(lcase, cname_len);
		} else {
			zend_str_tolower(lcase + cname_len - constant_name_len, constant_name_len);
		}
		cname = lcase;
		if ((*constant = zend_hash_str_find_ptr(EG(zend_constants), cname, cname_len)) == NULL || ((*constant)->flags & CONST_CS)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Constant %s not found", old_cname);
			efree(lcase);
			return FAILURE;
		}
	}
	if (found_cname) {
		*found_cname = lcase ? lcase : estrndup(cname, cname_len);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_update_children_consts
	Scan the class_table for children of the class just updated */
int php_runkit_update_children_consts(RUNKIT_53_TSRMLS_ARG(zval *pDest), int num_args, va_list args, zend_hash_key *hash_key)
{
	// TODO: Assert type is IS_PTR or the CE internal type

	zend_class_entry *ce = Z_CE_P(pDest);
	zend_class_entry *parent_class =  va_arg(args, zend_class_entry*);
	zval *c = va_arg(args, zval*);
	zend_string* cname = va_arg(args, zend_string*);

	ce = *((zend_class_entry**)ce);

	if (ce->parent != parent_class) {
		/* Not a child, ignore */
		return ZEND_HASH_APPLY_KEEP;
	}

	/* Process children of this child */
	zend_hash_apply_with_arguments(RUNKIT_53_TSRMLS_PARAM(EG(class_table)), php_runkit_update_children_consts, 3, ce, c, cname);

	Z_ADDREF_P(c);

	zend_hash_del(&ce->constants_table, cname);
	if (zend_hash_add(&ce->constants_table, cname, c) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error updating child class");
		return ZEND_HASH_APPLY_KEEP;
	}

	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{{ php_runkit_constant_remove
 */
static int php_runkit_constant_remove(zend_string* classname, zend_string* constname TSRMLS_DC)
{
	zend_constant *constant;
	char *found_constname;

	if (classname && ZSTR_LEN(classname) > 0) {
		zend_class_entry *ce;

		if (php_runkit_fetch_class(classname, &ce TSRMLS_CC)==FAILURE) {
			return FAILURE;
		}

		if (!zend_hash_exists(&ce->constants_table, constname)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Constant %s::%s does not exist", ZSTR_VAL(classname), ZSTR_VAL(constname));
			return FAILURE;
		}
		if (zend_hash_del(&ce->constants_table, constname)==FAILURE) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove constant %s::%s", ZSTR_VAL(classname), ZSTR_VAL(constname));
			return FAILURE;
		}
		php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);
		return SUCCESS;
	}

	if (php_runkit_fetch_const(constname, &constant, &found_constname TSRMLS_CC) == FAILURE) {
		return FAILURE;
	}

	if (constant->module_number != PHP_USER_CONSTANT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Only user defined constants may be removed.");
		return FAILURE;
	}

	if (zend_hash_str_del(EG(zend_constants), found_constname, ZSTR_LEN(constant->name)) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove constant");
		efree(found_constname);
		return FAILURE;
	}
	efree(found_constname);

	php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_constant_add
 */
static int php_runkit_constant_add(zend_string* classname, zend_string* constname, zval *value TSRMLS_DC)
{
	zend_class_entry *ce;
	zval copyval;

	switch (Z_TYPE_P(value)) {
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
		case IS_TRUE:
		case IS_FALSE:
		case IS_RESOURCE:
		case IS_NULL:
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Constants may only evaluate to scalar values");
			return FAILURE;
	}

	if ((classname == NULL) || (ZSTR_LEN(classname) == 0)) {
		zend_constant c;

		// TODO: Memory management properly
		if (ZSTR_LEN(constname) > 0 && ZSTR_VAL(constname)[0] == '\\') {
			constname = zend_string_init(ZSTR_VAL(constname) + 1, ZSTR_LEN(constname) - 1, 0);
		} else {
			zend_string_addref(constname);
		}

		/* Traditional global constant */
		c.value = *value;
		zval_copy_ctor(&c.value);
		c.flags = CONST_CS;
		c.name = constname;
		c.module_number = PHP_USER_CONSTANT;
		return zend_register_constant(&c TSRMLS_CC);
	}

	if (php_runkit_fetch_class(classname, &ce TSRMLS_CC)==FAILURE) {
		return FAILURE;
	}

	copyval = *value;
	zval_copy_ctor(&copyval);
	if (zend_hash_add(&ce->constants_table, constname, &copyval) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to add constant to class definition");
		zval_ptr_dtor(&copyval);
		return FAILURE;
	}

	php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);

	zend_hash_apply_with_arguments(RUNKIT_53_TSRMLS_PARAM(EG(class_table)), (apply_func_args_t)php_runkit_update_children_consts, 3, ce, &copyval, constname);

	return SUCCESS;
}
/* }}} */

/* *****************
   * Constants API *
   ***************** */

/* {{{ proto bool runkit_constant_redefine(string constname, mixed newvalue)
 */
PHP_FUNCTION(runkit_constant_redefine)
{
	zend_string *classname, *constname;
	zval *value;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Sz", &constname, &value) == FAILURE) {
		RETURN_FALSE;
	}

	PHP_RUNKIT_SPLIT_PN(classname, constname);

	php_runkit_constant_remove(classname, constname TSRMLS_CC);
	RETURN_BOOL(php_runkit_constant_add(classname, constname, value TSRMLS_CC) == SUCCESS);
}
/* }}} */

/* {{{ proto bool runkit_constant_remove(string constname)
 */
PHP_FUNCTION(runkit_constant_remove)
{
	zend_string *classname, *constname;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "S", &constname) == FAILURE) {
		RETURN_FALSE;
	}

	PHP_RUNKIT_SPLIT_PN(classname, constname);

	// TODO: memory management here, and everywhere else using PHP_RUNKIT_SPLIT_PN
	RETURN_BOOL(php_runkit_constant_remove(classname, constname TSRMLS_CC)==SUCCESS);
}
/* }}} */

/* {{{ proto bool runkit_constant_add(string constname, mixed value)
	Similar to define(), but allows defining in class definitions as well
 */
PHP_FUNCTION(runkit_constant_add)
{
	zend_string *classname, *constname;
	zval *value;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Sz", &constname, &value) == FAILURE) {
		RETURN_FALSE;
	}

	PHP_RUNKIT_SPLIT_PN(classname, constname);

	RETURN_BOOL(php_runkit_constant_add(classname, constname, value TSRMLS_CC) == SUCCESS);
}
/* }}} */
#endif /* PHP_RUNKIT_MANIPULATION_CONSTANTS */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

