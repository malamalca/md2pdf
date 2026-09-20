#!/usr/bin/php -q
<?php
/**
 * md2pdf CLI entry point
 */

define('DS', DIRECTORY_SEPARATOR);
require dirname(__DIR__) . DS . 'config' . DS . 'paths.php';
require ROOT . DS . 'vendor' . DS . 'autoload.php';

use App\Command\ConvertCommand;

// Load config
$config = require CONFIG . 'app.php';

// Run command
$command = new ConvertCommand($config);

$status = $command->run($argv[1] ?? null, ...array_slice($argv, 2));
exit($status);
