#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use IO::Select;
use Test::Nginx qw/ :DEFAULT http_start /;

my $t = Test::Nginx->new()->has(qw/http/)->plan(3);

$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
master_process off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    log_format order $uri;
    access_log %%TESTDIR%%/order.log order;
    sendfile off;
    sendfile_max_chunk 64k;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        zstd on;
        zstd_comp_level 9;
        zstd_buffers 2 128k;
        location /big.html { output_buffers 1 2m; }
    }
}
EOF

my $block = join '', map {
	"line $_: common text with some changing content " . ($_ * 7919) . "\n"
} 0..15999;
open my $file, '>', $t->testdir . '/big.html' or die "open large file: $!";
print $file $block for 1..128;
close $file or die "close large file: $!";
$t->write_file('small.txt', 'healthy');
$t->run();

my $big = http_start("GET /big.html HTTP/1.0\r\nHost: localhost\r\n"
	. "Accept-Encoding: zstd\r\n\r\n");
my $sel = IO::Select->new($big);
my $first = '';
$big->sysread($first, 65536) if $sel->can_read(5);
like($first, qr/Content-Encoding: zstd/, 'large response compressed');

my $small = http_start("GET /small.txt HTTP/1.0\r\nHost: localhost\r\n\r\n");
$sel->add($small);
my $body = '';
my $deadline = time + 20;
while ($sel->count && time < $deadline) {
	my @ready = $sel->can_read(1);
	for my $fh (@ready) {
		my $n = $fh->sysread(my $buf, 65536);
		die "read response: $!" if !defined $n;
		$body .= $buf if $fh == $small;
		$sel->remove($fh) if !$n;
	}
}
if ($sel->count) {
	my $pid = $t->read_file('nginx.pid');
	kill 'KILL', $pid or die "kill hung nginx: $!";
	waitpid($pid, 0);
	BAIL_OUT('responses did not complete');
}
close $big;
close $small;
like($body, qr/\r\n\r\nhealthy\z/, 'small response intact');
$t->stop();
is($t->read_file('order.log'), "/small.txt\n/big.html\n",
	'other connection handled before compression completes');