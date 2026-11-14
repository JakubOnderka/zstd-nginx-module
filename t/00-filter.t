#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use IPC::Open3;
use Symbol qw/gensym/;
use Test::Nginx qw/ :DEFAULT http_content /;

my $t = Test::Nginx->new()->has(qw/http/)->plan(16);

$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    zstd on;
    zstd_types text/plain;
    zstd_comp_level 3;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location /off/ { zstd off; }
        location /short/ { zstd_min_length 1000000; }
        location /level/ { zstd_comp_level 5; zstd_buffers 2 4k; }
        location /html-only/ { zstd_types application/json; }
        location /proxy/ { proxy_pass http://127.0.0.1:8080/; }
    }
}
EOF

my $content = join '', map { "line $_: " . ('abcdef' x 30) . "\n" } 1..3000;
for my $path ('off', 'short', 'level', 'html-only') {
	mkdir $t->testdir . "/$path" or die "mkdir $path: $!";
}
for my $path ('data.txt', 'off/data.txt', 'short/data.txt',
	'level/data.txt', 'html-only/data.txt')
{
	$t->write_file($path, $content);
}
$t->run();

my $r = request('/data.txt', 'zstd');
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'compressed response');
is(decompress($t, $r, 'filter'), $content, 'response decompresses without a dictionary');
unlike($r, qr/^Content-Length:/mi, 'compressed response has no content length');

$r = request('/data.txt', undef);
unlike($r, qr/^Content-Encoding:/mi, 'no compression without accept encoding');
is(http_content($r), $content, 'uncompressed content');

$r = request('/data.txt', 'gzip');
unlike($r, qr/^Content-Encoding: zstd/mi, 'no compression for gzip only');

$r = request('/off/data.txt', 'zstd');
unlike($r, qr/^Content-Encoding:/mi, 'disabled in location');
is(http_content($r), $content, 'disabled content');

$r = request('/short/data.txt', 'zstd');
unlike($r, qr/^Content-Encoding:/mi, 'minimum length');
is(http_content($r), $content, 'minimum length content');

$r = request('/html-only/data.txt', 'zstd');
unlike($r, qr/^Content-Encoding:/mi, 'unconfigured content type');

$r = request('/level/data.txt', 'gzip, zstd');
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'location compression level');
is(decompress($t, $r, 'level'), $content, 'location response decompresses');

$r = request('/proxy/data.txt', 'zstd');
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'proxied response compressed');
is(decompress($t, $r, 'proxy'), $content, 'proxied response decompresses');

$r = http("HEAD /data.txt HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: zstd\r\n\r\n");
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'HEAD has compressed representation headers');

sub request {
	my ($uri, $encoding) = @_;
	my $header = defined $encoding ? "Accept-Encoding: $encoding\r\n" : '';
	return http("GET $uri HTTP/1.0\r\nHost: localhost\r\n${header}\r\n");
}

sub decompress {
	my ($t, $response, $name) = @_;
	my $dir = $t->testdir;
	$t->write_file("$name.zst", http_content($response));
	system('zstd', '-q', '-d', '-f', "$dir/$name.zst", '-o', "$dir/$name.out") == 0
		or die "zstd decompression failed";
	open my $fh, '<', "$dir/$name.out" or die "open decompressed file: $!";
	binmode $fh;
	return do { local $/; <$fh> };
}