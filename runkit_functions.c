/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
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

/* $Id$ */

#include "php_runkit.h"
#include "php_runkit_hash.h"


#ifdef PHP_RUNKIT_MANIPULATION

/* {{{ php_runkit_check_call_stack
 */
int php_runkit_check_call_stack(zend_op_array *op_array TSRMLS_DC)
{
	zend_execute_data *ptr;

	ptr = EG(current_execute_data);

	while (ptr) {
		// TODO: ???
		if (ptr->func && ptr->func->op_array.opcodes == op_array->opcodes) {
			return FAILURE;
		}
		ptr = ptr->prev_execute_data;
	}

	return SUCCESS;
}
/* }}} */

/* Temporary function name. This function is manipulated with functions in this file. */
#define RUNKIT_TEMP_FUNCNAME  "__runkit_temporary_function__"

/* Maintain order */
#define PHP_RUNKIT_FETCH_FUNCTION_INSPECT	0
#define PHP_RUNKIT_FETCH_FUNCTION_REMOVE	1
#define PHP_RUNKIT_FETCH_FUNCTION_RENAME	2

/* {{{ php_runkit_fetch_function
 */
static zend_function* php_runkit_fetch_function(zend_string* fname, int flag TSRMLS_DC)
{
	zend_function *fe;
	zend_string* fname_lower;

	fname_lower = zend_string_tolower(fname);

	if ((fe = zend_hash_find_ptr(EG(function_table), fname_lower)) == NULL) {
		zend_string_release(fname_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s() not found", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type == ZEND_INTERNAL_FUNCTION &&
		!RUNKIT_G(internal_override)) {
		zend_string_release(fname_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s() is an internal function and runkit.internal_override is disabled", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type != ZEND_USER_FUNCTION &&
		fe->type != ZEND_INTERNAL_FUNCTION) {
		zend_string_release(fname_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s() is not a user or normal internal function", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type == ZEND_INTERNAL_FUNCTION &&
		flag >= PHP_RUNKIT_FETCH_FUNCTION_REMOVE) {
		zend_string* runkit_key;

		if (!RUNKIT_G(replaced_internal_functions)) {
			ALLOC_HASHTABLE(RUNKIT_G(replaced_internal_functions));
			zend_hash_init(RUNKIT_G(replaced_internal_functions), 4, NULL, NULL, 0);
		}
		// FIXME figure out what this is intended to do (Restoring internal functions?)
		zend_hash_add_ptr(RUNKIT_G(replaced_internal_functions), fname, fe);
		/*
		 * If internal functions have been modified then runkit's request shutdown handler
		 * should be called after all other modules' ones.
		 */
		runkit_key = zend_string_init("runkit", sizeof("runkit") - 1, 0);
		php_runkit_hash_move_to_front(&module_registry, php_runkit_hash_get_bucket(&module_registry, runkit_key));
		zend_string_release(runkit_key);
		EG(full_tables_cleanup) = 1; // dirty hack!
	}
	zend_string_release(fname_lower);

	return fe;
}
/* }}} */

/* {{{ php_runkit_ensure_misplaced_internal_functions_table_exists */
static inline void php_runkit_ensure_misplaced_internal_functions_table_exists() {
	if (!RUNKIT_G(misplaced_internal_functions)) {
		ALLOC_HASHTABLE(RUNKIT_G(misplaced_internal_functions));
		zend_hash_init(RUNKIT_G(misplaced_internal_functions), 4, NULL, NULL, 0);
	}
}
/* }}} */

/* {{{ php_runkit_add_to_misplaced_internal_functions */
static inline void php_runkit_add_to_misplaced_internal_functions(zend_function *fe, zend_string *name_lower TSRMLS_DC) {
	if (fe->type == ZEND_INTERNAL_FUNCTION &&
	    (!RUNKIT_G(misplaced_internal_functions) ||
	     !zend_hash_exists(RUNKIT_G(misplaced_internal_functions), name_lower))
	) {
		zval tmp;
		php_runkit_ensure_misplaced_internal_functions_table_exists();
		ZVAL_STR(&tmp, name_lower);
		zend_hash_next_index_insert(RUNKIT_G(misplaced_internal_functions), &tmp);
	}
}
/* }}} */

/* {{{ php_runkit_destroy_misplaced_internal_function */
static inline void php_runkit_destroy_misplaced_internal_function(zend_function *fe, zend_string *fname_lower TSRMLS_DC) {
	if (fe->type == ZEND_INTERNAL_FUNCTION && RUNKIT_G(misplaced_internal_functions) &&
	    zend_hash_exists(RUNKIT_G(misplaced_internal_functions), fname_lower)) {
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(fe);
		zend_hash_del(RUNKIT_G(misplaced_internal_functions), fname_lower);
	}
}
/* }}} */

/** {{{ php_runkit_set_opcode_constant
		for absolute constant addresses, creates a local copy of that literal.
		Modifies op's contents. */
static void php_runkit_set_opcode_constant(const zval* literals, znode_op* op, zval* literalI) {
	debug_printf("php_runkit_set_opcode_constant(%llx, %llx, %d)", (long long)literals, (long long)literalI, (int)sizeof(zval));
	// TODO: ZEND_PASS_TWO_UPDATE_CONSTANT???
#if ZEND_USE_ABS_CONST_ADDR
	RT_CONSTANT_EX(literals, *op) = literalI;
#else
	// TODO: Assert that this is in a meaningful range.
	// TODO: is this ever necessary for relative constant addresses?
	op->constant = literalI - literals;
#endif
}
/* }}} */

/* {{{ php_runkit_function_copy_ctor
	Duplicate structures in an op_array where necessary to make an outright duplicate */
void php_runkit_function_copy_ctor(zend_function *fe, zend_string* newname TSRMLS_DC)
{
	zval *literals;
	void *run_time_cache;
	zend_string **dupvars;  // Array of zend_string*s
	zend_op *last_op;
	zend_op *opcode_copy;
	uint32_t i;

	if (newname) {
		zend_string_addref(newname);
		fe->common.function_name = newname;
	} else {
		zend_string_addref(fe->common.function_name);
		// TODO: is this a memory leak?
		// fe->common.function_name = fe->common.function_name;
	}

	if (fe->common.type == ZEND_USER_FUNCTION) {
		if (fe->op_array.vars) {
			i = fe->op_array.last_var;
			dupvars = safe_emalloc(fe->op_array.last_var, sizeof(zend_string*), 0);
			while (i > 0) {
				i--;
				dupvars[i] = fe->op_array.vars[i];
				zend_string_addref(dupvars[i]);
			}
			fe->op_array.vars = dupvars;
		}

		if (fe->op_array.static_variables) {
			// Similar to zend_compile.c's zend_create_closure copying static variables, zend_compile.c's do_bind_function
#if PHP_VERSION_ID >= 70100
			fe->op_array.static_variables = zend_array_dup(fe->op_array.static_variables);
#else
			HashTable *static_variables = fe->op_array.static_variables;
			ALLOC_HASHTABLE(fe->op_array.static_variables);
			zend_hash_init(fe->op_array.static_variables, zend_hash_num_elements(static_variables), NULL, ZVAL_PTR_DTOR, 0);
			zend_hash_apply_with_arguments(RUNKIT_53_TSRMLS_PARAM(static_variables), zval_copy_static_var, 1, fe->op_array.static_variables);
#endif
		}

		if (fe->op_array.run_time_cache) {
			// TODO: Garbage collect these, somehow?
			run_time_cache = pemalloc(fe->op_array.cache_size, 1);
			memcpy(run_time_cache, fe->op_array.run_time_cache, fe->op_array.cache_size);
			fe->op_array.run_time_cache = run_time_cache;
		}

		opcode_copy = safe_emalloc(sizeof(zend_op), fe->op_array.last, 0);
		last_op = fe->op_array.opcodes + fe->op_array.last;
		// TODO: See if this code works on 32-bit PHP.
		for(i = 0; i < fe->op_array.last; i++) {
			opcode_copy[i] = fe->op_array.opcodes[i];
			debug_printf("opcode = %s, is_const=%d, constant=%d\n", zend_get_opcode_name((int)opcode_copy[i].opcode), (int) (opcode_copy[i].op1_type == IS_CONST), fe->op_array.opcodes[i].op1.constant);
			if (opcode_copy[i].op1_type == IS_CONST) {
			} else {
				zend_op *opline;
				zend_op *jmp_addr_op1;
				switch (opcode_copy[i].opcode) {
#ifdef ZEND_GOTO
					case ZEND_GOTO:
#endif
#ifdef ZEND_FAST_CALL
					case ZEND_FAST_CALL:
#endif
					case ZEND_JMP:
						opline = &opcode_copy[i];
						jmp_addr_op1 = OP_JMP_ADDR(opline, opline->op1);
						// TODO: I don't know if this is really the same as jmp_addr. FIXME
						if (jmp_addr_op1 >= fe->op_array.opcodes &&
							jmp_addr_op1 < last_op) {
#if ZEND_USE_ABS_JMP_ADDR
							opline->op1.jmp_addr = opcode_copy + (fe->op_array.opcodes[i].op1.jmp_addr - fe->op_array.opcodes);
#else
							opline->op1.jmp_offset += ((char*)fe->op_array.opcodes) - ((char*)opcode_copy);
#endif
						}
				}
			}
			if (opcode_copy[i].op2_type == IS_CONST) {
			} else {
				zend_op *opline;
				zend_op *jmp_addr_op2;
				switch (opcode_copy[i].opcode) {
					case ZEND_JMPZ:
					case ZEND_JMPNZ:
					case ZEND_JMPZ_EX:
					case ZEND_JMPNZ_EX:
#ifdef ZEND_JMP_SET
					case ZEND_JMP_SET:
#endif
#ifdef ZEND_JMP_SET_VAR
					case ZEND_JMP_SET_VAR:
#endif
						opline = &opcode_copy[i];
						jmp_addr_op2 = OP_JMP_ADDR(opline, opline->op2);
						// TODO: I don't know if this is really the same as jmp_addr. FIXME
						if (jmp_addr_op2 >= fe->op_array.opcodes &&
							jmp_addr_op2 < last_op) {
#if ZEND_USE_ABS_JMP_ADDR
							opline->op2.jmp_addr = opcode_copy + (fe->op_array.opcodes[i].op2.jmp_addr - fe->op_array.opcodes);
#else
							opline->op2.jmp_offset += ((char*)fe->op_array.opcodes) - ((char*)opcode_copy);
#endif
						}
				}
			}
		}

		debug_printf("op_array.literals = %llx, last_literal = %d\n", (long long)fe->op_array.literals, fe->op_array.last_literal);
		if (fe->op_array.literals) {
			i = fe->op_array.last_literal;
			literals = safe_emalloc(fe->op_array.last_literal, sizeof(zval), 0);
			while (i > 0) {
				uint32_t k;

				i--;
				// This code copies the zvals from the original function's op array to the new function's op array.
				// TODO: Re-examine this line to see if reference counting was done properly.
				// TODO: fix all of the other places old types of copies were used.
				literals[i] = fe->op_array.literals[i];
				Z_TRY_ADDREF(literals[i]);
				debug_printf("Copying literal of type %d\n", (int) Z_TYPE(literals[i]));
				// TODO: Check if constants are being replaced properly.
				// TODO: This may no longer do anything on 64-bit builds of PHP.
				for (k=0; k < fe->op_array.last; k++) {
					debug_printf("%d(%s)\ttype=%d %d\n", k, zend_get_opcode_name(opcode_copy[k].opcode), (int)opcode_copy[k].op1_type, (int)opcode_copy[k].op2_type);
					if (opcode_copy[k].op1_type == IS_CONST) {
						debug_printf("op1: %llx, %llx\n", (long long)RT_CONSTANT_EX(literals, opcode_copy[k].op1), (long long)&(fe->op_array.literals[i]));
					}
					if (opcode_copy[k].op2_type == IS_CONST) {
						debug_printf("op2: %llx, %llx\n", (long long)RT_CONSTANT_EX(literals, opcode_copy[k].op2), (long long)&(fe->op_array.literals[i]));
					}
					debug_printf("old constant = %d\n", opcode_copy[k].op1.constant);
					// TODO: This may be completely unnecessary on 64-bit systems. This may be broken on 32-bit systems.
					if (opcode_copy[k].op1_type == IS_CONST && RT_CONSTANT_EX(literals, opcode_copy[k].op1) == &fe->op_array.literals[i]) {
						php_runkit_set_opcode_constant(literals, &(opcode_copy[k].op1), &literals[i]);
					}
					if (opcode_copy[k].op2_type == IS_CONST && RT_CONSTANT_EX(literals, opcode_copy[k].op2) == &fe->op_array.literals[i]) {
						php_runkit_set_opcode_constant(literals, &(opcode_copy[k].op2), &literals[i]);
					}
				}
			}
			fe->op_array.literals = literals;
		}

		fe->op_array.opcodes = opcode_copy;
		fe->op_array.refcount = (uint32_t*) emalloc(sizeof(uint32_t));
		*fe->op_array.refcount = 1;
		// TODO: Check if this is an interned string...
		if (fe->op_array.doc_comment) {
			zend_string_addref(fe->op_array.doc_comment);
		}
		fe->op_array.try_catch_array = (zend_try_catch_element*)estrndup((char*)fe->op_array.try_catch_array, sizeof(zend_try_catch_element) * fe->op_array.last_try_catch);
#if PHP_MAJOR_VERSION == 7 && PHP_MINOR_VERSION == 0
	fe->op_array.brk_cont_array = (zend_brk_cont_element*)estrndup((char*)fe->op_array.brk_cont_array, sizeof(zend_brk_cont_element) * fe->op_array.last_brk_cont);
#endif

		if (fe->op_array.arg_info) {
			zend_arg_info *tmpArginfo;
			zend_arg_info *originalArginfo;
			// num_args calculation taken from zend_opcode.c destroy_op_array
			// TODO: Add tests that functions with return types are properly created and destroyed.
			// TODO: Specify what runkit should do about return types, what is an error, what is valid.
			uint32_t num_args = fe->op_array.num_args;
			int32_t offset = 0;

			if (fe->op_array.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
				offset++;
				num_args++;
			}
			if (fe->op_array.fn_flags & ZEND_ACC_VARIADIC) {
				num_args++;
			}

			tmpArginfo = (zend_arg_info*) safe_emalloc(sizeof(zend_arg_info), num_args, 0);

			originalArginfo = &((fe->op_array.arg_info)[-offset]);
			for (i = 0; i < num_args; i++) {
				tmpArginfo[i] = originalArginfo[i];
				if (tmpArginfo[i].name) {
					zend_string_addref((tmpArginfo[i].name));
				}
				if (tmpArginfo[i].class_name) {
					zend_string_addref(tmpArginfo[i].class_name);
				}
			}
			fe->op_array.arg_info = &tmpArginfo[offset];
		}
	}

	fe->common.fn_flags &= ~ZEND_ACC_DONE_PASS_TWO;
	fe->common.prototype = fe;
}
/* }}} */

/* {{{ php_runkit_function_clone
   Makes a duplicate of fe that doesn't share any static variables, zvals, etc.
   TODO: Is there anything I can use from zend_duplicate_function? */
zend_function* php_runkit_function_clone(zend_function *fe, zend_string *newname TSRMLS_DC) {
	// Make a persistent allocation.
	// TODO: Clean it up after a request?
	zend_function *new_function = pemalloc(sizeof(zend_function), 1);
	memcpy(new_function, fe, sizeof(zend_function));
	php_runkit_function_copy_ctor(new_function, newname TSRMLS_CC);
	return new_function;
}
/* }}} */

/* {{{ php_runkit_function_dtor */
void php_runkit_function_dtor(zend_function *fe TSRMLS_DC) {
	zval zv;
	ZVAL_FUNC(&zv, fe);
	zend_function_dtor(&zv);
	// Note: This can only be used with zend_functions created by php_runkit_function_clone.
	// ZEND_INTERNAL_FUNCTIONs are freed.
	if (fe->type == ZEND_USER_FUNCTION) {
		pefree(fe, 1);
	}
}
/* }}} */

/* {{{ php_runkit_clear_function_runtime_cache */
static void php_runkit_clear_function_runtime_cache(zend_function *f)
{
	if (f->type != ZEND_USER_FUNCTION ||
	    f->op_array.cache_size == 0 || f->op_array.run_time_cache == NULL) {
		return;
	}

	// TODO: Does memset do what I want it to do?
	memset(f->op_array.run_time_cache, 0, f->op_array.cache_size);
}
/* }}} */

/* {{{ php_runkit_clear_function_runtime_cache_for_function_table */
static void php_runkit_clear_function_runtime_cache_for_function_table(HashTable *function_table) {
	zend_function* f;
	ZEND_HASH_FOREACH_PTR(function_table, f) {
		php_runkit_clear_function_runtime_cache(f);
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ php_runkit_clear_all_functions_runtime_cache */
void php_runkit_clear_all_functions_runtime_cache(TSRMLS_D)
{
	uint32_t i;
	zend_execute_data *ptr;
	zend_class_entry* ce;

	php_runkit_clear_function_runtime_cache_for_function_table(EG(function_table));

	ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
		php_runkit_clear_function_runtime_cache_for_function_table(&(ce->function_table));
	} ZEND_HASH_FOREACH_END();

	// TODO: Does this make sense, does it work with a runtime cache?
	for (ptr = EG(current_execute_data); ptr != NULL; ptr = ptr->prev_execute_data) {
		// TODO: I assume that ptr->run_time_cache is the same pointer, if set?
		if (ptr->func == NULL || ptr->func->type == ZEND_INTERNAL_FUNCTION || ptr->func->op_array.cache_size == 0 || ptr->func->op_array.run_time_cache == NULL) {
			continue;
		}
		memset(ptr->func->op_array.run_time_cache, 0, ptr->func->op_array.cache_size);
	}

	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_BEGIN(i)
		if (object->ce == zend_ce_closure) {
			zend_closure *cl = (zend_closure *) object;
			php_runkit_clear_function_runtime_cache(&cl->func);
		}
	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_END
}
/* }}} */

/* {{{ php_runkit_reflection_update_property */
static void php_runkit_reflection_update_property(zval* object, const char* name, zval* value) {
	// Copied from ext/reflection's reflection_update_property
	zval member;
	ZVAL_STRING(&member, name);
	zend_std_write_property(object, &member, value, NULL);
	if (Z_REFCOUNTED_P(value)) Z_DELREF_P(value);
	zval_ptr_dtor(&member);
}
/* }}} */

/* {{{ php_runkit_update_reflection_object_name */
void php_runkit_update_reflection_object_name(zend_object* object, int handle, const char* name) {
	zval obj, prop_value;
	ZVAL_OBJ(&obj, object);
	ZVAL_STRING(&prop_value, name);
	php_runkit_reflection_update_property(&obj, RUNKIT_G(name_str), &prop_value);
}
/* }}} */

/* {{{ php_runkit_free_reflection_function */
static void php_runkit_free_reflection_function(zend_function *fptr) {
	// Exact copy of ext/reflection's _free_function
	if (fptr
		&& (fptr->internal_function.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE))
	{
		zend_string_release(fptr->internal_function.function_name);
		zend_free_trampoline(fptr);
	}
}
/* }}} */

/* {{{ reflection_object_from_obj
   Copied from ext/reflection/php_reflection.c */
static inline reflection_object *reflection_object_from_obj(zend_object *obj) {
	return (reflection_object*)((char*)(obj) - XtOffsetOf(reflection_object, zo));
}
/* }}} */

/* {{{ php_runkit_delete_reflection_function_ptr
 	Frees the parts of a reflection object referring to the removed method/function(/parameter?)  */
static void php_runkit_delete_reflection_function_ptr(reflection_object *intern TSRMLS_DC) {
	// Copied from ext/reflection's reflection_free_objects_storage
	parameter_reference *reference;
	if (intern->ptr) {
		switch(intern->ref_type) {
			case REF_TYPE_PARAMETER:
				reference = (parameter_reference*)intern->ptr;
				php_runkit_free_reflection_function(reference->fptr);
				efree(intern->ptr);
				break;
			case REF_TYPE_FUNCTION:
				php_runkit_free_reflection_function(intern->ptr);
				break;
			case REF_TYPE_PROPERTY:
				efree(intern->ptr);
				break;
			default:
				php_error_docref(NULL TSRMLS_CC, E_ERROR, "Attempted to free ReflectionObject of unexpected REF_TYPE %d\n", (int) (intern->ref_type));
				return;
		}
	}
	intern->ptr = NULL;
	// Do NOT call object destructors yet - ReflectionObject will do that in __destruct.
}
/* }}}*/

/* {{{ php_runkit_remove_function_from_reflection_objects */
void php_runkit_remove_function_from_reflection_objects(zend_function *fe TSRMLS_DC) {
	uint32_t i;
	extern PHPAPI zend_class_entry *reflection_function_ptr;
	extern PHPAPI zend_class_entry *reflection_method_ptr;
	extern PHPAPI zend_class_entry *reflection_parameter_ptr;

	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_BEGIN(i)
		if (object->ce == reflection_function_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
			if (refl_obj->ptr == fe) {
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = RUNKIT_G(removed_function);
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_function_str));
			}
		} else if (object->ce == reflection_method_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
			if (refl_obj->ptr == fe) {
				zend_function *f = emalloc(sizeof(zend_function));
				memcpy(f, RUNKIT_G(removed_method), sizeof(zend_function));
				f->common.scope = fe->common.scope;
#ifdef ZEND_ACC_CALL_VIA_HANDLER
				f->internal_function.fn_flags |= ZEND_ACC_CALL_VIA_HANDLER; // This is a trigger to free it from destructor
#endif
				zend_string_addref(f->internal_function.function_name);
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = f;
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_method_str));
			}
		} else if (object->ce == reflection_parameter_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
			parameter_reference *reference = (parameter_reference *) refl_obj->ptr;
			if (reference && reference->fptr == fe) {
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = NULL;
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_parameter_str));
			}
		}
	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_END
}
/* }}} */

/* {{{ php_runkit_generate_lambda_method
	Heavily borrowed from ZEND_FUNCTION(create_function) */
int php_runkit_generate_lambda_method(const zend_string *arguments, const zend_string *phpcode,
                                      zend_function **pfe, zend_bool return_ref TSRMLS_DC)
{
	char *eval_code, *eval_name;
	int eval_code_length;

	eval_code_length = sizeof("function " RUNKIT_TEMP_FUNCNAME) + ZSTR_LEN(arguments) + 4 + ZSTR_LEN(phpcode) + return_ref;
	eval_code = (char*)emalloc(eval_code_length);
	snprintf(eval_code, eval_code_length, "function %s" RUNKIT_TEMP_FUNCNAME "(%s){%s}", (return_ref ? "&" : ""), ZSTR_VAL(arguments), ZSTR_VAL(phpcode));
	eval_name = zend_make_compiled_string_description("runkit runtime-created function" TSRMLS_CC);
	if (zend_eval_string(eval_code, NULL, eval_name TSRMLS_CC) == FAILURE) {
		efree(eval_code);
		efree(eval_name);
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Cannot create temporary function");
		zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1);
		return FAILURE;
	}
	efree(eval_code);
	efree(eval_name);

	if ((*pfe = zend_hash_str_find_ptr(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Unexpected inconsistency during create_function");
		return FAILURE;
	}

	return SUCCESS;
}

/* }}} */

/** {{{ php_runkit_cleanup_lambda_method
    Tries to free the temporary lambda function. If it fails, emits a warning and returns FAILURE.  */
int php_runkit_cleanup_lambda_method() {
	if (zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove temporary function entry");
		return FAILURE;
	}
	return SUCCESS;
}
/* }}}*/

/* {{{ php_runkit_destroy_misplaced_functions
	Wipe old internal functions that were renamed to new targets
	They'll get replaced soon enough */
int php_runkit_destroy_misplaced_functions(zval *zv TSRMLS_DC)
{
	zend_function *fe;
	if (Z_TYPE_P(zv) != IS_STRING || Z_STRLEN_P(zv) == 0) {
		/* Nonsense, skip it */
		return ZEND_HASH_APPLY_REMOVE;
	}

	if ((fe = zend_hash_find_ptr(EG(function_table), Z_STR_P(zv))) != NULL) {
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(fe);
	}

	// TODO: Valgrind complained about invalid reads if this is called for an internal function such as val_dump
	// Try to figure out how/when to properly delete functions, especially if references are shared.
	zend_hash_del(EG(function_table), Z_STR_P(zv));

	return ZEND_HASH_APPLY_REMOVE;
}
/* }}} */

/* {{{ php_runkit_restore_internal_functions
	Cleanup after modifications to internal functions */
int php_runkit_restore_internal_functions(RUNKIT_53_TSRMLS_ARG(zval *pDest), int num_args, va_list args, zend_hash_key *hash_key)
{
	// TODO: Expect IS_PTR
	zend_function* fe_raw = Z_FUNC_P(pDest);
	// TODO: Expect IS_INTERNAL_FUNCTION

	if (!ZSTR_LEN(hash_key->key)) {
		/* Nonsense, skip it */
		return ZEND_HASH_APPLY_REMOVE;
	}

	zend_hash_update_ptr(EG(function_table), hash_key->key, fe_raw);

	/* It's possible for restored internal functions to now be blocking a ZEND_USER_FUNCTION
	 * which will screw up post-request cleanup.
	 * Avoid this by restoring internal functions to the front of the list where they won't be in the way
	 */
	// TODO: Need to fix move_to_front
	php_runkit_hash_move_to_front(EG(function_table), php_runkit_hash_get_bucket(EG(function_table), hash_key->key));

	return ZEND_HASH_APPLY_REMOVE;
}
/* }}} */

/* {{{ php_runkit_function_add_or_update */
static void php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAMETERS, int add_or_update) {
	zend_string* funcname;
	zend_string* funcname_lower;
	zend_string* arguments = NULL;
	zend_string* phpcode = NULL;
	zend_string* doc_comment = NULL;  // TODO: Is this right?
	zend_bool return_ref = 0;
	zend_function *orig_fe = NULL, *source_fe = NULL, *func;
	zval *args;
	int remove_temp = 0;
	long argc = ZEND_NUM_ARGS();
	long opt_arg_pos = 2;

	if (argc < 1 || zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, 1 TSRMLS_CC, "S", &funcname) == FAILURE || !ZSTR_LEN(funcname)) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Function name should not be empty");
		RETURN_FALSE;
	}

	if (argc < 2) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Function body should be provided");
		RETURN_FALSE;
	}

	if (!php_runkit_parse_args_to_zvals(argc, &args TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (!php_runkit_parse_function_arg(argc, args, 1, &source_fe, &arguments, &phpcode, &opt_arg_pos, "Function" TSRMLS_CC)) {
		efree(args);
		RETURN_FALSE;
	}

	if (argc > opt_arg_pos && !source_fe) {
		switch (Z_TYPE(args[opt_arg_pos])) {
			case IS_NULL:
			case IS_STRING:
			case IS_LONG:
			case IS_DOUBLE:
			case IS_TRUE:
			case IS_FALSE:
				convert_to_boolean_ex(&args[opt_arg_pos]);
				return_ref = Z_TYPE(args[opt_arg_pos]) == IS_TRUE;
				break;
			default:
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "return_ref should be boolean");
		}
		opt_arg_pos++;
	}

	doc_comment = php_runkit_parse_doc_comment_arg(argc, args, opt_arg_pos TSRMLS_CC);
	efree(args);

	if (add_or_update == HASH_UPDATE &&
	    (orig_fe = php_runkit_fetch_function(funcname, PHP_RUNKIT_FETCH_FUNCTION_REMOVE TSRMLS_CC)) == NULL) {
		RETURN_FALSE;
	}

	/* UTODO */
	funcname_lower = zend_string_tolower(funcname);

	if (add_or_update == HASH_ADD && zend_hash_exists(EG(function_table), funcname_lower)) {
		zend_string_release(funcname_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Function %s() already exists", ZSTR_VAL(funcname));
		RETURN_FALSE;
	}

	if (!source_fe) {
		if (php_runkit_generate_lambda_method(arguments, phpcode, &source_fe, return_ref TSRMLS_CC) == FAILURE) {
			zend_string_release(funcname_lower);
			RETURN_FALSE;
		}
		remove_temp = 1;
	}

	func = php_runkit_function_clone(source_fe, funcname TSRMLS_CC);
	func->common.scope = NULL;
	func->common.fn_flags &= ~ZEND_ACC_CLOSURE;

	if (doc_comment == NULL && source_fe->op_array.doc_comment == NULL &&
	    orig_fe && orig_fe->type == ZEND_USER_FUNCTION && orig_fe->op_array.doc_comment) {
		doc_comment = orig_fe->op_array.doc_comment;
	}
	php_runkit_modify_function_doc_comment(func, doc_comment);

	if (add_or_update == HASH_UPDATE) {
		php_runkit_remove_function_from_reflection_objects(orig_fe TSRMLS_CC);
		php_runkit_destroy_misplaced_internal_function(orig_fe, funcname_lower TSRMLS_CC);

		php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);
	}

	if (runkit_zend_hash_add_or_update_ptr(EG(function_table), funcname_lower, func, add_or_update) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to add new function");
		zend_string_release(funcname_lower);
		if (remove_temp && zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1) == FAILURE) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove temporary function entry");
		}
		// TODO: Is there a chance this will accidentally delete the original function?
		php_runkit_function_dtor(func);
		RETURN_FALSE;
	}

	if (remove_temp && zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove temporary function entry");
	}

	if (zend_hash_find(EG(function_table), funcname_lower) == NULL) {
		// TODO: || !fe - what did that do?
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to locate newly added function");
		zend_string_release(funcname_lower);
		RETURN_FALSE;
	}

	zend_string_release(funcname_lower);

	RETURN_TRUE;
}
/* }}} */

/* *****************
   * Functions API *
   ***************** */

/* {{{  proto bool runkit_function_add(string funcname, string arglist, string code[, bool return_by_reference=NULL[, string doc_comment]])
	proto bool runkit_function_add(string funcname, closure code[, string doc_comment])
	Add a new function, similar to create_function, but allows specifying name
	*/
PHP_FUNCTION(runkit_function_add)
{
	php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_ADD);
}
/* }}} */

/* {{{ proto bool runkit_function_remove(string funcname)
 */
PHP_FUNCTION(runkit_function_remove)
{
	zend_string *fname;
	zend_string *fname_lower;
	int result;
	zend_function *fe;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "S", &fname) == FAILURE) {
		RETURN_FALSE;
	}

	// TODO what?
	if ((fe = php_runkit_fetch_function(fname, PHP_RUNKIT_FETCH_FUNCTION_REMOVE TSRMLS_CC)) == NULL) {
		RETURN_FALSE;
	}

	fname_lower = zend_string_tolower(fname);

	php_runkit_remove_function_from_reflection_objects(fe TSRMLS_CC);
	php_runkit_destroy_misplaced_internal_function(fe, fname_lower TSRMLS_CC);

	result = (zend_hash_del(EG(function_table), fname_lower) == SUCCESS);
	zend_string_release(fname_lower);

	php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);

	RETURN_BOOL(result);
}
/* }}} */

/* {{{ PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS */
#define PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS \
	zend_string *sfunc; \
	zend_string *dfunc; \
	zend_string *sfunc_lower; \
	zend_string *dfunc_lower; \
	\
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, \
			"SS", \
			&sfunc, \
			&dfunc) == FAILURE ) { \
		RETURN_FALSE; \
	} \
	\
	/* UTODO */ \
	dfunc_lower = zend_string_tolower(dfunc); \
	\
	if (zend_hash_exists(EG(function_table), dfunc_lower)) { \
		zend_string_release(dfunc_lower); \
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s() already exists", ZSTR_VAL(dfunc)); \
		RETURN_FALSE; \
	}
/* }}} */

/* {{{ ensure_misplaced_internal_functions_table_exists */
static void ensure_misplaced_internal_functions_table_exists() {
	if (!RUNKIT_G(misplaced_internal_functions)) {
		ALLOC_HASHTABLE(RUNKIT_G(misplaced_internal_functions));
		zend_hash_init(RUNKIT_G(misplaced_internal_functions), 4, NULL, NULL, 0);
	}
}
/* }}} */

/* {{{record_misplaced_internal_function */
static void record_misplaced_internal_function(zend_string* fname_lower) {
	zval tmp;
	ZVAL_STR(&tmp, fname_lower);
	ensure_misplaced_internal_functions_table_exists();
	zend_hash_next_index_insert(RUNKIT_G(misplaced_internal_functions), &tmp);
}
/* }}} */

/* {{{ proto bool runkit_function_rename(string funcname, string newname)
 */
PHP_FUNCTION(runkit_function_rename)
{
	zend_function *func, *sfe;
	ZEND_RESULT_CODE deleted;
	PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS;

	if ((sfe = php_runkit_fetch_function(sfunc, PHP_RUNKIT_FETCH_FUNCTION_RENAME TSRMLS_CC)) == NULL) {
		zend_string_release(dfunc_lower);
		RETURN_FALSE;
	}

	sfunc_lower = zend_string_tolower(sfunc);

	php_runkit_remove_function_from_reflection_objects(sfe TSRMLS_CC);
	php_runkit_destroy_misplaced_internal_function(sfe, sfunc_lower TSRMLS_CC);

	// TODO: Should I use clone instead?
	// This may cause errors.
	// TODO: this was calling the destructor of EG(function_table)
	// I want to extract the function, and want this to not happen.
	if (sfe->type == ZEND_INTERNAL_FUNCTION) {
		func = sfe;
	} else {
		func = php_runkit_function_clone(sfe, dfunc TSRMLS_CC);
	}

	if (sfe == func) {
		dtor_func_t orig = EG(function_table)->pDestructor;
		EG(function_table)->pDestructor = NULL;
		deleted = zend_hash_del(EG(function_table), sfunc_lower) == FAILURE;
		EG(function_table)->pDestructor = orig;
	} else {
		deleted = zend_hash_del(EG(function_table), sfunc_lower) == FAILURE;
	}

	if (deleted) {
		zend_string_release(dfunc_lower);
		zend_string_release(sfunc_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error removing reference to old function name %s()", ZSTR_VAL(sfunc));
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(func);
		/* TODO: is this needed?
		ZVAL_FUNC(&tmpVal, &func);
		zend_function_dtor(&tmpVal);
		*/
		RETURN_FALSE;
	}
	zend_string_release(sfunc_lower);

	if (zend_hash_add_ptr(EG(function_table), dfunc_lower, func) == NULL) {
		zend_string_release(dfunc_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to add reference to new function name %s()", ZSTR_VAL(dfunc));
		// TODO: Figure out if this is needed?
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(func);
		// TODO: I'm not sure if this line will delete the zvals/strings of the original function,
		// which would cause segfaults.
		// cloning might make more sense above.
		if (func->type == ZEND_USER_FUNCTION) {
			zval tmp;
			ZVAL_FUNC(&tmp, func);
			zend_function_dtor(&tmp);
		}
		RETURN_FALSE;
	}

	php_runkit_add_to_misplaced_internal_functions(func, dfunc_lower);

	zend_string_release(dfunc_lower);

	php_runkit_clear_all_functions_runtime_cache(TSRMLS_C);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool runkit_function_redefine(string funcname, string arglist, string code[, bool return_by_reference=NULL[, string doc_comment]])
 *     proto bool runkit_function_redefine(string funcname, closure code[, string doc_comment])
 */
PHP_FUNCTION(runkit_function_redefine)
{
	php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_UPDATE);
}
/* }}} */

/* {{{ proto bool runkit_function_copy(string funcname, string targetname)
 */
PHP_FUNCTION(runkit_function_copy)
{
	zend_function *fe, *sfe;
	PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS;

	if ((sfe = php_runkit_fetch_function(sfunc, PHP_RUNKIT_FETCH_FUNCTION_INSPECT TSRMLS_CC)) == NULL) {
		zend_string_release(dfunc_lower);
		RETURN_FALSE;
	}

	sfunc_lower = zend_string_tolower(sfunc);

	if (sfe->type == ZEND_USER_FUNCTION) {
		fe = php_runkit_function_clone(sfe, dfunc TSRMLS_CC);
	} else {
		record_misplaced_internal_function(dfunc_lower);

		// TODO: Should I copy the new name for backtraces?
		fe = pemalloc(sizeof(zend_function), 1);
		memcpy(fe, sfe, sizeof(zend_function));
	}
	php_runkit_add_to_misplaced_internal_functions(fe, dfunc_lower TSRMLS_CC);

	// TODO: Does this even make sense to add?

	if (zend_hash_add_ptr(EG(function_table), dfunc_lower, fe) == NULL) {
		zend_string_release(dfunc_lower);
		zend_string_release(sfunc_lower);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to add reference to new function name %s()", ZSTR_VAL(dfunc));
		// TODO: figure out if this is necessary
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(fe);
		php_runkit_function_dtor(fe);
		RETURN_FALSE;
	}

	zend_string_release(dfunc_lower);
	zend_string_release(sfunc_lower);

	RETURN_TRUE;

}
/* }}} */
#endif /* PHP_RUNKIT_MANIPULATION */

#if PHP_VERSION_ID >= 70100
# define RETURN_VALUE_USED(opline) ((opline)->result_type != IS_UNUSED)
#else
# define RETURN_VALUE_USED(opline) !((opline)->result_type & EXT_TYPE_UNUSED)
#endif

/* {{{ proto bool runkit_return_value_used(void)
Does the calling function do anything with our return value? */
PHP_FUNCTION(runkit_return_value_used)
{
    // This is still broken. Look into what vld does.
	zend_execute_data *ptr = EX(prev_execute_data);

	if (!ptr) {
		/* main() */
		RETURN_FALSE;
	}

	RETURN_BOOL(RETURN_VALUE_USED(ptr->opline));
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

