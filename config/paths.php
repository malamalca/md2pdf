<?php
/**
 * Path constants
 */
if (!defined('DS')) {
    define('DS', DIRECTORY_SEPARATOR);
}

define('ROOT', dirname(__DIR__));
define('CONFIG', ROOT . DS . 'config' . DS);
define('TEMPLATES', ROOT . DS . 'templates' . DS);
define('TMP', sys_get_temp_dir() . DS);
