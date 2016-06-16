/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group, (c) 2008-2015 Dmitry Zenovich |
  +----------------------------------------------------------------------+
  | This source file is subject to the new BSD license,                  |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.opensource.org/licenses/BSD-3-Clause                      |
  | If you did not receive a copy of the license and are unable to       |
  | obtain it through the world-wide-web, please send a note to          |
  | dzenovich@gmail.com so we can mail you a copy immediately.           |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  | Modified by Dmitry Zenovich <dzenovich@gmail.com>                    |
  +----------------------------------------------------------------------+
*/

/* TODO specify and fix the way this deals with private, protected, and public consts. */

/* $Id$ */

#include "php_runkit.h"
#include "php_runkit_zval.h"

// TODO: Actually implement this.
#ifdef PHP_RUNKIT_MANIPULATION
/* {{{ php_runkit_import_functions
 */
static int php_runkit_import_functions(HashTable *function_table, long flags
                                       , zend_bool *clear_cache
                                       TSRMLS_DC)
{
	zend_function *fe;
	zend_ulong idx;
	zend_string *key;

	ZEND_HASH_FOREACH_KEY_PTR(function_table, idx, key, fe) {
		zend_function *orig_fe;
		zend_string *new_key;
		zend_bool add_function = 1;
		zend_bool exists = 0;

		new_key = fe->common.function_name;

		if (fe && fe->type == ZEND_USER_FUNCTION) {

			if (key != NULL) {
				new_key = key;
				exists = ((orig_fe = zend_hash_find_ptr(EG(function_table), new_key)) != NULL);
			} else {
				exists = ((orig_fe = zend_hash_index_find_ptr(EG(function_table), idx)) != NULL);
			}

			if (exists) {
				php_runkit_remove_function_from_reflection_objects(orig_fe TSRMLS_CC);
				if (flags & PHP_RUNKIT_IMPORT_OVERRIDE) {
					if (key != NULL) {
						if (zend_hash_del(EG(function_table), new_key) == FAILURE) {
							php_error_docref(NULL TSRMLS_CC, E_WARNING, "Inconsistency cleaning up import environment");
							return FAILURE;
						}
					} else {
						if (zend_hash_index_del(EG(function_table), idx) == FAILURE) {
							php_error_docref(NULL TSRMLS_CC, E_WARNING, "Inconsistency cleaning up import environment");
							return FAILURE;
						}
					}
					*clear_cache = 1;
				} else {
					add_function = 0;
				}
			}
		}

		if (add_function) {
			if (zend_hash_add_ptr(EG(function_table), new_key, fe) == NULL) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure importing %s()", ZSTR_VAL(fe->common.function_name));
				PHP_RUNKIT_DESTROY_FUNCTION(fe);
				return FAILURE;
			} else {
				PHP_RUNKIT_FUNCTION_ADD_REF(fe);
			}
		}
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_methods
 */
static int php_runkit_import_class_methods(zend_class_entry *dce, zend_class_entry *ce, int override
                                           , zend_bool *clear_cache
                                           TSRMLS_DC)
{
	zend_function *fe;

	ZEND_HASH_FOREACH_PTR(&ce->function_table, fe) {
		zend_function *dfe = NULL;
		zend_class_entry *fe_scope;
		/* TODO: check for memory leaks */
		zend_string * const fn = zend_string_tolower(fe->common.function_name);  // TODO: copy?

		fe_scope = php_runkit_locate_scope(ce, fe, fn);

		if (fe_scope != ce) {
			zend_string_release(fn);
			/* This is an inherited function, let's skip it */
			continue;
		}

		dfe = zend_hash_find_ptr(&dce->function_table, fn);
		if (dfe != NULL) {
			zend_class_entry *scope;
			if (!override) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::%s() already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}

			scope = php_runkit_locate_scope(dce, dfe, fn);

			if (php_runkit_check_call_stack(&dfe->op_array TSRMLS_CC) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot override active method %s::%s(). Skipping.", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}

			*clear_cache = 1;

			php_runkit_clean_children_methods_foreach(RUNKIT_53_TSRMLS_PARAM(EG(class_table)), scope, dce, fn, dfe);
			php_runkit_remove_function_from_reflection_objects(dfe TSRMLS_CC);
			if (zend_hash_del(&dce->function_table, fn) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error removing old method in destination class %s::%s", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}
		}

		fe->common.scope = dce;
		if (zend_hash_add_ptr(&dce->function_table, fn, fe) == NULL) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure importing %s::%s()", ZSTR_VAL(ce->name), ZSTR_VAL(fe->common.function_name));
			zend_string_release(fn);
			continue;
		} else {
			PHP_RUNKIT_FUNCTION_ADD_REF(fe);
		}

		if ((fe = zend_hash_find_ptr(&dce->function_table, fn)) == NULL) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot get newly created method %s::%s()", ZSTR_VAL(ce->name), ZSTR_VAL(fn));
			zend_string_release(fn);
			continue;
		}
		PHP_RUNKIT_ADD_MAGIC_METHOD(dce, fn, fe, dfe TSRMLS_CC);
		php_runkit_update_children_methods_foreach(RUNKIT_53_TSRMLS_PARAM(EG(class_table)),
		                               dce, dce, fe, fn, dfe);

		zend_string_release(fn);
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_consts
 */
static int php_runkit_import_class_consts(zend_class_entry *dce, zend_class_entry *ce, int override TSRMLS_DC)
{
	zend_string *key;
	zval c;

	ZEND_HASH_FOREACH_STR_KEY(&ce->constants_table, key) {
		long action = HASH_ADD;

		if (key != NULL) {
			if (zend_hash_exists(&dce->constants_table, key)) {
				if (override) {
					action = HASH_UPDATE;
				} else {
					php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::%s already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(key));
					continue;
				}
			}

			php_runkit_zval_resolve_class_constant(&c, dce TSRMLS_CC);
			Z_TRY_ADDREF(c);

			if (zend_hash_add_or_update(&dce->constants_table, key, &c, action) == NULL) {
				Z_TRY_DELREF(c);
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s::%s", ZSTR_VAL(dce->name), ZSTR_VAL(key));
			}

			php_runkit_update_children_consts_foreach(RUNKIT_53_TSRMLS_PARAM(EG(class_table)), dce, &c, key);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Constant has invalid key name");
		}
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_static_props
 */
static int php_runkit_import_class_static_props(zend_class_entry *dce, zend_class_entry *ce, int override, int remove_from_objects TSRMLS_DC)
{
	zend_string *key;
	zend_property_info *property_info_ptr;


	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, key, property_info_ptr) {
		zval *p;
		zend_property_info *ex_property_info_ptr;
		if (key == NULL) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Property has invalid key name");
			continue;
		}
		if (!(property_info_ptr->flags & ZEND_ACC_STATIC)) {
			continue;
		}
		p = &CE_STATIC_MEMBERS(ce)[property_info_ptr->offset];
		if ((ex_property_info_ptr = zend_hash_find_ptr(&dce->properties_info, key)) != NULL) {
			if (override) {
				if (php_runkit_def_prop_remove_int(dce, key, NULL, 0, 0, NULL TSRMLS_CC) != SUCCESS) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s::$%s (cannot remove old member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
					continue;
				}
				php_runkit_zval_resolve_class_constant(p, dce TSRMLS_CC);
				if (php_runkit_def_prop_add_int(dce, key, p, property_info_ptr->flags,
												property_info_ptr->doc_comment, dce,
												override, remove_from_objects TSRMLS_CC) != SUCCESS) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s::$%s (cannot add new member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
					continue;
				}
				continue;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::$%s already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			}
		} else {
			if (php_runkit_def_prop_add_int(dce, key, p, property_info_ptr->flags,
											property_info_ptr->doc_comment,
											dce,
											override, remove_from_objects TSRMLS_CC) != SUCCESS) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s::$%s (cannot add new member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_props
 */
static int php_runkit_import_class_props(zend_class_entry *dce, zend_class_entry *ce, int override, int remove_from_objects TSRMLS_DC)
{
	zend_string *key;

	zend_property_info *property_info_ptr;

	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, key, property_info_ptr) {
		zval *p;
		// TODO: Check if this was a property_info_ptr

		// Ignore non-string keys
		if (key == NULL) {
			continue;
		}

		if (property_info_ptr->flags & ZEND_ACC_STATIC) {
			continue;
		}
		if (zend_hash_exists(&dce->properties_info, key)) {
			if (!override) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s%s%s already exists, not importing",
								 ZSTR_VAL(dce->name), (property_info_ptr->flags & ZEND_ACC_STATIC) ? "::$" : "->", ZSTR_VAL(key));
				continue;
			}
		}
		p = &ce->default_properties_table[property_info_ptr->offset];
		// TODO: what does this do?
		// TODO: Figure out if this is the correct replacement - copied from add_class_vars
		// if ((p_src = zend_hash_find(&ce->default_properties, property_info_ptr->name)) == NULL)
		if (Z_TYPE_P(p) == IS_UNDEF) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
							 "Cannot import broken default property %s->%s", ZSTR_VAL(ce->name), ZSTR_VAL(key));
			continue;
		}

		// TODO: Fix constant check
		if (Z_CONSTANT_P(p)) {
			// TODO check return code?
			zval_update_constant_ex(p, dce TSRMLS_CC);
		}

		php_runkit_zval_resolve_class_constant(p, dce TSRMLS_CC);
		php_runkit_def_prop_add_int(dce, key, p,
									property_info_ptr->flags, property_info_ptr->doc_comment,
									dce, override, remove_from_objects TSRMLS_CC);
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_classes
 */
static int php_runkit_import_classes(HashTable *class_table, long flags
                                     , zend_bool *clear_cache
                                     TSRMLS_DC)
{
	zval *ce_zv;
	zend_string *key;

	ZEND_HASH_FOREACH_STR_KEY_VAL(class_table, key, ce_zv) {
		zend_class_entry *ce = NULL;
		zend_class_entry *dce;

		if (Z_TYPE_P(ce_zv) != IS_PTR) {
			php_error_docref(NULL TSRMLS_CC, E_ERROR, "Non-class in class table!");
			return FAILURE;
		}
		ce = Z_PTR_P(ce_zv);
		if (ce == NULL) {
			php_error_docref(NULL TSRMLS_CC, E_ERROR, "Null class in class table!");
		}

		if (ce->type != ZEND_USER_CLASS) {
			if (ce->type == ZEND_INTERNAL_CLASS) {
				php_error_docref(NULL TSRMLS_CC, E_ERROR, "Cannot import internal class!");
			} else {
				php_error_docref(NULL TSRMLS_CC, E_ERROR, "Corrupt class table?");
			}
			return FAILURE;
		}

		if (key != NULL) {
			zend_string *classname = ce->name;
			zend_string *classname_lower = zend_string_tolower(classname);

			if (!zend_hash_exists(EG(class_table), classname_lower)) {
				php_runkit_class_copy(ce, ce->name TSRMLS_CC);
			}
			zend_string_release(classname_lower);

			if ((dce = php_runkit_fetch_class(ce->name TSRMLS_CC)) == NULL) {
				/* Oddly non-existant target class or error retreiving it... Or it's an internal class... */
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot redeclare class %s", ZSTR_VAL(ce->name));
				continue;
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_CONSTS) {
				php_runkit_import_class_consts(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) TSRMLS_CC);
			}
			if (flags & PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS) {
				php_runkit_import_class_static_props(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) != 0,
				                                     (flags & PHP_RUNKIT_OVERRIDE_OBJECTS) != 0
				                                     TSRMLS_CC);
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_PROPS) {
				php_runkit_import_class_props(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) != 0,
				                              (flags & PHP_RUNKIT_OVERRIDE_OBJECTS) != 0
				                              TSRMLS_CC);
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_METHODS) {
				php_runkit_import_class_methods(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE)
				                                , clear_cache
				                                TSRMLS_CC);
			}
		}
	} ZEND_HASH_FOREACH_END();

	// TODO: blindly assuming that temporary classes are cleaned up properly in zend_hash_destroy.
	// See if this is wrong.

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_compile_filename
 * Duplicate of Zend's compile_filename which explicitly calls the internal compile_file() implementation.
 *
 * This is only used when an accelerator has replaced zend_compile_file() with an alternate method
 * which has been known to cause issues with overly-optimistic early binding.
 *
 * It would be cleaner to temporarily set zend_compile_file back to compile_file, but that wouldn't be
 * particularly thread-safe so.... */
static zend_op_array *php_runkit_compile_filename(int type, zval *filename TSRMLS_DC)
{
	zend_file_handle file_handle;
	zval tmp;
	zend_op_array *retval;
	zend_string *opened_path = NULL;

	if (Z_TYPE_P(filename) != IS_STRING) {
		tmp = *filename;
		zval_copy_ctor(&tmp);
		convert_to_string(&tmp);
		filename = &tmp;
	}
	file_handle.filename = filename->value.str->val;
	file_handle.free_filename = 0;
	file_handle.type = ZEND_HANDLE_FILENAME;
	file_handle.opened_path = NULL;
	file_handle.handle.fp = NULL;

	/* Use builtin compiler only -- bypass accelerators and whatnot */
	retval = compile_file(&file_handle, type TSRMLS_CC);
	if (retval && file_handle.handle.stream.handle) {
		// TODO: Duplicate instead?
		if (!file_handle.opened_path) {
			file_handle.opened_path = opened_path = zend_string_copy(Z_STR_P(filename));
		}

		zend_hash_add_empty_element(&EG(included_files), file_handle.opened_path);

		if (opened_path) {
			zend_string_release(opened_path);
		}
	}
	zend_destroy_file_handle(&file_handle TSRMLS_CC);

	if (filename==&tmp) {
		zval_dtor(&tmp);
	}
	return retval;
}
/* }}} */

static HashTable *current_class_table, *tmp_class_table, *tmp_eg_class_table, *current_eg_class_table, *current_function_table, *tmp_function_table;
static uint32_t php_runkit_old_compiler_options;
void (*php_runkit_old_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);

/* {{{ php_runkit_error_cb */
void php_runkit_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args) {
	TSRMLS_FETCH();

	zend_error_cb = php_runkit_old_error_cb;
	CG(class_table) = current_class_table;
	EG(class_table) = current_eg_class_table;
	CG(function_table) = current_function_table;
	CG(compiler_options) = php_runkit_old_compiler_options;

	php_runkit_old_error_cb(type, error_filename, error_lineno, format, args);
}
/* }}} */

/* {{{ array runkit_import(string filename[, long flags])
	Import functions and class definitions from a file
	Similar to include(), but doesn't execute root op_array, and allows pre-existing functions/methods to be overridden */
PHP_FUNCTION(runkit_import)
{
	zend_op_array *new_op_array;
	zval *filename;
	long flags = PHP_RUNKIT_IMPORT_CLASS_METHODS;
	zend_bool clear_cache = 0;

	zend_op_array *(*local_compile_filename)(int type, zval *filename TSRMLS_DC) = compile_filename;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|l", &filename, &flags) == FAILURE) {
		RETURN_FALSE;
	}
	convert_to_string(filename);

	if (compile_file != zend_compile_file) {
		/* An accelerator or other dark force is at work
		 * Use the wrapper method to force the builtin compiler
		 * to be used */
		local_compile_filename = php_runkit_compile_filename;
	}

	tmp_class_table = (HashTable *) emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_class_table, 10, NULL, ZEND_CLASS_DTOR, 0, 0);
	tmp_eg_class_table = (HashTable *) emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_eg_class_table, 10, NULL, ZEND_CLASS_DTOR, 0, 0);
	tmp_function_table = (HashTable *) emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_function_table, 100, NULL, ZEND_FUNCTION_DTOR, 0, 0);

	current_class_table = CG(class_table);
	CG(class_table) = tmp_class_table;
	current_eg_class_table = EG(class_table);
	EG(class_table) = tmp_eg_class_table;
	current_function_table = CG(function_table);
	CG(function_table) = tmp_function_table;
	php_runkit_old_error_cb = zend_error_cb;
	zend_error_cb = php_runkit_error_cb;
	php_runkit_old_compiler_options = CG(compiler_options);
	CG(compiler_options) |= ZEND_COMPILE_DELAYED_BINDING;

	new_op_array = local_compile_filename(ZEND_INCLUDE, filename TSRMLS_CC);

	zend_error_cb = php_runkit_old_error_cb;
	CG(class_table) = current_class_table;
	EG(class_table) = current_eg_class_table;
	CG(function_table) = current_function_table;
	CG(compiler_options) = php_runkit_old_compiler_options;

	if (!new_op_array) {
		zend_hash_destroy(tmp_class_table);
		efree(tmp_class_table);
		zend_hash_destroy(tmp_eg_class_table);
		efree(tmp_eg_class_table);
		zend_hash_destroy(tmp_function_table);
		efree(tmp_function_table);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Import Failure");
		RETURN_FALSE;
	}

	// This is similar to, but not identical to the function zend_do_delayed_early_binding.
	// However, the superclasses will be found in the original class table,
	// but the new classes are found in a separate, temporary class table.
	if (new_op_array->early_binding != (uint32_t)-1) {
		zend_bool orig_in_compilation = CG(in_compilation);
		uint32_t opline_num = new_op_array->early_binding;
		zend_class_entry *pce;

		CG(in_compilation) = 1;
		while (opline_num != (uint32_t)-1) {
			// TODO: Check if it's still op2, figure out the set of expected opcodes
			zval *op2_zv = RT_CONSTANT_EX(new_op_array->literals, new_op_array->opcodes[opline_num - 1].op2);

			if (Z_TYPE_P(op2_zv) == IS_STRING && (pce = php_runkit_fetch_class_int(Z_STR_P(op2_zv))) != NULL) {
				do_bind_inherited_class(new_op_array, &new_op_array->opcodes[opline_num], tmp_class_table, pce, 0 TSRMLS_CC);
			}
			opline_num = new_op_array->opcodes[opline_num].result.opline_num;
		}
		CG(in_compilation) = orig_in_compilation;
	}

	/* We never really needed the main loop opcodes to begin with */
	destroy_op_array(new_op_array TSRMLS_CC);
	efree(new_op_array);

	if (flags & PHP_RUNKIT_IMPORT_FUNCTIONS) {
		php_runkit_import_functions(tmp_function_table, flags
		                            , &clear_cache
		                            TSRMLS_CC);
	}

	if (flags & PHP_RUNKIT_IMPORT_CLASSES) {
		php_runkit_import_classes(tmp_class_table, flags
		                          , &clear_cache
		                          TSRMLS_CC);
	}

	zend_hash_destroy(tmp_class_table);
	efree(tmp_class_table);
	zend_hash_destroy(tmp_eg_class_table);
	efree(tmp_eg_class_table);
	zend_hash_destroy(tmp_function_table);
	efree(tmp_function_table);

	if (clear_cache) {
		php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);
	}

	RETURN_TRUE;
}
/* }}} */
#endif /* PHP_RUNKIT_MANIPULATION */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
