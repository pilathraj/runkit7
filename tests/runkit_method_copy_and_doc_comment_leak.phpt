--TEST--
runkit_method_copy() function and doc_comment - test if one call to copy will leak memory
--SKIPIF--
<?php if(!extension_loaded("runkit") || !RUNKIT_FEATURE_MANIPULATION) print "skip"; ?>
--INI--
display_errors=on
--FILE--
<?php
ini_set('error_reporting', E_ALL);

class runkit_class {
	/**
	 * old doc_comment
	 */
	function runkit_method($a) {
		echo "a is $a\n";
	}
}
runkit_method_redefine('runkit_class','runkit_method','', '', NULL, 'new doc_comment');
$r = new ReflectionMethod('runkit_class', 'runkit_method');
echo $r->getDocComment(), "\n";
echo "After redefine\n";
runkit_method_redefine('runkit_class','runkit_method','', '', NULL, 'redefined doc_comment');
echo $r->getDocComment(), "\n";
?>
--EXPECT--
new doc_comment
After redefine
redefined doc_comment
redefined doc_comment
