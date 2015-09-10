--TEST--
runkit_default_property_add() function - instance override
--SKIPIF--
<?php if(!extension_loaded("runkit") || !RUNKIT_FEATURE_MANIPULATION) print "skip";
      if(array_shift(explode('.', PHP_VERSION)) < 5) print "skip";
?>
--INI--
error_reporting=E_ALL
display_errors=on
--FILE--
<?php
class RunkitClass {
}

$className = 'RunkitClass';
print_r(new $className());
runkit_default_property_add($className, 'theAnswer', 42);
print_r(new $className());
?>
--EXPECTF--
RunkitClass Object
(
)
RunkitClass Object
(
    [theAnswer] => 42
)

