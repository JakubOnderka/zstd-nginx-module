#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use IO::Socket::INET;
use Test::Nginx qw/ :DEFAULT http_content /;

# A short upstream body must not trap the filter on an empty flush buffer.
# https://github.com/tokers/zstd-nginx-module/pull/23

plan tests => 26;

for my $case (['on', 'truncated'], ['on', 'chunked'],
	['off', 'truncated'], ['off', 'chunked'])
{
	my ($buffering, $kind) = @$case;
	my $t = Test::Nginx->new()->has(qw/http proxy/);
	$t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
daemon off;
master_process off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server {
        listen 127.0.0.1:8080;
        zstd on;
        zstd_min_length 0;
        zstd_types application/json;
        location / {
            proxy_pass http://127.0.0.1:8081;
            proxy_buffering $buffering;
            proxy_read_timeout 2s;
        }
        location = /health { return 200 healthy; }
    }
}
EOF

	$t->run_daemon(\&upstream);
	$t->waitforsocket('127.0.0.1:' . port(8081))
		or die "upstream did not start";
	$t->run();

	my $name = "$kind, buffering $buffering";
	my $r = request("/$kind");
	ok(defined $r, "$name: response terminates");
	like($r, qr/^Content-Encoding: zstd\r?$/mi,
		"$name: response compressed");
	if ($kind eq 'truncated') {
		like($t->read_file('error.log'), qr/upstream prematurely closed connection/,
			"$name: premature upstream close detected");
		ok(defined $r && $r !~ /\r\n0\r\n\r\n\z/,
			"$name: incomplete response has no final HTTP chunk");
	} elsif (defined $r) {
		is(decompress($t, $r), 'a' x 32768 . 'b' x 32768,
			"$name: response decompresses");
	} else {
		fail("$name: response decompresses");
	}

	my $health = defined $r ? http_get('/health') : undef;
	like($health, qr/\r\n\r\nhealthy\z/, "$name: worker remains responsive");
	if (!defined $health) {
		# master_process off makes this the only nginx process to clean up.
		my $pid = $t->read_file('nginx.pid');
		kill 'KILL', $pid or die "kill hung nginx $pid: $!";
		waitpid($pid, 0);
	}

	$t->stop();
	$t->stop_daemons();
}

sub request {
	my ($uri) = @_;
	return http("GET $uri HTTP/1.1\r\nHost: localhost\r\n"
		. "Accept-Encoding: zstd\r\nConnection: close\r\n\r\n");
}

sub decompress {
	my ($t, $response) = @_;
	my $dir = $t->testdir;
	$t->write_file('response.zst', http_content($response));
	system('zstd', '-q', '-d', '-f', "$dir/response.zst", '-o', "$dir/response.out") == 0
		or die "zstd decompression failed";
	return $t->read_file('response.out');
}

sub upstream {
	my $server = IO::Socket::INET->new(
		Proto => 'tcp',
		LocalAddr => '127.0.0.1:' . port(8081),
		Listen => 5,
		Reuse => 1
	) or die "create upstream socket: $!";

	local $SIG{PIPE} = 'IGNORE';

	while (my $client = $server->accept()) {
		$client->autoflush(1);
		my $request = '';
		while (my $line = <$client>) {
			$request .= $line;
			last if $line =~ /^\r?\n$/;
		}
		if ($request =~ m{^GET /truncated }) {
			print $client "HTTP/1.1 200 OK\r\n"
				. "Content-Type: application/json\r\n"
				. "Content-Length: 2\r\nConnection: close\r\n\r\na";
		} elsif ($request =~ m{^GET /chunked }) {
			print $client "HTTP/1.1 200 OK\r\n"
				. "Content-Type: application/json\r\n"
				. "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
			for my $chunk ('a' x 32768, 'b' x 32768) {
				print $client sprintf("%x\r\n", length $chunk), $chunk, "\r\n";
				select undef, undef, undef, 0.1;
			}
			print $client "0\r\n\r\n";
		}
		close $client;
	}
}
