#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use Digest::SHA qw/sha256 sha256_hex/;
use Test::Nginx qw/ :DEFAULT http_content /;

my $t = Test::Nginx->new()->has(qw/http/)->plan(28);

$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
master_process off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server {
        listen 127.0.0.1:8080;
        default_type application/octet-stream;
        zstd on;
        zstd_types application/octet-stream;
        zstd_buffers 8 1k;
        sendfile off;
        location /single/ {
            alias %%TESTDIR%%/;
            output_buffers 1 2m;
        }
        location /multi/ {
            alias %%TESTDIR%%/;
            output_buffers 1 16k;
        }
    }
}
EOF

# Force output backpressure while a last_buf still has unconsumed input.
my $content = join '', map { sha256(pack('N', $_)) } 1..46875;
my @sizes = (131071, 131072, 131073, 300000, 1500000);
for my $size (@sizes) {
	$t->write_file("$size.bin", substr($content, 0, $size));
}
$t->write_file('repeat.bin', 'a' x 300000);
$t->run();

for my $case ((map { ["single/$_.bin", substr($content, 0, $_)] } @sizes),
	['single/repeat.bin', 'a' x 300000],
	['multi/300000.bin', substr($content, 0, 300000)])
{
	my ($uri, $expected) = @$case;
	my $r = http("GET /$uri HTTP/1.0\r\nHost: localhost\r\n"
		. "Accept-Encoding: zstd\r\n\r\n");
	if (!defined $r) {
		my $pid = $t->read_file('nginx.pid');
		kill 'KILL', $pid or die "kill hung nginx $pid: $!";
		waitpid($pid, 0);
		BAIL_OUT("$uri: response timed out");
	}
	my ($headers) = split /\r\n\r\n/, $r, 2;
	like($headers, qr/^Content-Encoding: zstd\r?$/mi, "$uri: compressed response");
	my $dir = $t->testdir;
	$t->write_file('response.zst', http_content($r));
	my $status = system('zstd', '-q', '-d', '-f', "$dir/response.zst",
		'-o', "$dir/response.out");
	is($status, 0, "$uri: complete zstd frame");
	my $decoded = $t->read_file('response.out');
	is(length $decoded, length $expected, "$uri: full response length");
	is(sha256_hex($decoded), sha256_hex($expected), "$uri: exact response content");
}