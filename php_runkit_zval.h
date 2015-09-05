#ifndef PHP_RUNKIT_ZVAL_H
#define PHP_RUNKIT_ZVAL_H

/* {{{ php_runkit_zval_resolve_class_constant */
inline static void php_runkit_zval_resolve_class_constant(zval *p, zend_class_entry *ce TSRMLS_DC) {
	if (Z_TYPE_P(p) == IS_CONSTANT_AST || Z_TYPE_P(p) == IS_CONSTANT) {
    // TODO: What does this do?
		zval_update_constant_ex(p, PHP_RUNKIT_CONSTANT_INDEX(1), ce TSRMLS_CC);
	}
}
/* }}} */


#define PHP_RUNKIT_ZVAL_CONVERT_TO_STRING_IF_NEEDED(val, val_copy) \
	if (Z_TYPE_P(member) != IS_STRING) { \
		val_copy = *val; \
		val = &val_copy; \
		zval_copy_ctor(val); \
		val->RUNKIT_REFCOUNT = 1; \
		convert_to_string(val); \
	}

#endif
