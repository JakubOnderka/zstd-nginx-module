#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use Test::Nginx qw/ :DEFAULT http_content /;

my $t = Test::Nginx->new()->has(qw/http/)->plan(10);

$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location /on/ { zstd_static on; }
        location /always/ { zstd_static always; }
        location /off/ { zstd_static off; }
    }
}
EOF

my $content = 'static content ' x 100;
for my $path ('on', 'always', 'off') {
	mkdir $t->testdir . "/$path" or die "mkdir $path: $!";
	$t->write_file("$path/data.txt", $content);
	my $dir = $t->testdir;
	system('zstd', '-q', '-f', "$dir/$path/data.txt", '-o', "$dir/$path/data.txt.zst") == 0
		or die "zstd compression failed";
}
$t->run();

my $r = request('/on/data.txt', 'zstd');
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'static on with zstd');
$t->write_file('response.zst', http_content($r));
my $dir = $t->testdir;
system('zstd', '-q', '-d', '-f', "$dir/response.zst", '-o', "$dir/response.out") == 0
	or die "zstd decompression failed";
open my $fh, '<', "$dir/response.out" or die "open decompressed file: $!";
is(do { local $/; <$fh> }, $content, 'static response decompresses');

$r = request('/on/data.txt', undef);
unlike($r, qr/^Content-Encoding:/mi, 'static on without accept encoding');
is(http_content($r), $content, 'static on returns original');

$r = request('/on/data.txt', 'gzip');
unlike($r, qr/^Content-Encoding:/mi, 'static on with gzip only');

$r = request('/always/data.txt', undef);
like($r, qr/^Content-Encoding: zstd\r?$/mi, 'static always without accept encoding');

$r = request('/off/data.txt', 'zstd');
unlike($r, qr/^Content-Encoding:/mi, 'static off');
is(http_content($r), $content, 'static off returns original');

like(request('/on/missing.txt', 'zstd'), qr/^HTTP\/1\.[01] 404 /, 'missing file');
like(request('/always/missing.txt', undef), qr/^HTTP\/1\.[01] 404 /, 'missing file with always');

sub request {
	my ($uri, $encoding) = @_;
	my $header = defined $encoding ? "Accept-Encoding: $encoding\r\n" : '';
	return http("GET $uri HTTP/1.0\r\nHost: localhost\r\n${header}\r\n");
}