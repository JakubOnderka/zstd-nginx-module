#!/usr/bin/perl

use warnings;
use strict;

use Test::More;
use IO::Select;
use IO::Socket::INET;
use IPC::Open3;
use Symbol qw/gensym/;
use Test::Nginx qw/ :DEFAULT http_start http_end http_content /;

my $t = Test::Nginx->new()->has(qw/http proxy debug/)->plan(20);

$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
master_process off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        zstd on;
        zstd_buffers 2 128k;
        location /level1/ { zstd_comp_level 1; }
        location /level3/ { zstd_comp_level 3; }
        location /slow {
            proxy_pass http://127.0.0.1:8081;
            proxy_buffering off;
        }
    }
}
EOF

my $payload = "worker cache integration, repeated text 0123456789\n" x 800;
for my $path ('level1', 'level3') {
	mkdir $t->testdir . "/$path" or die "mkdir $path: $!";
	$t->write_file("$path/body.html", $payload);
}
$t->run_daemon(\&upstream, $t->testdir, $payload);
$t->waitforsocket('127.0.0.1:' . port(8081))
	or die "upstream did not start";
$t->run();

my $first = request('/level1/body.html');
like($first, qr/^Content-Encoding: zstd\r?$/mi, 'first response compressed');
is(decompress($t, $first), $payload, 'first response content');

my $before = count_log($t, 'zstd: reusing worker cstream');
my $again = request('/level1/body.html');
ok(wait_log($t, 'zstd: reusing worker cstream', $before), 'worker context reused');
is(http_content($again), http_content($first), 'reused context produces same frame');
is(decompress($t, request('/level3/body.html')), $payload, 'different level decodes');
is(http_content(request('/level1/body.html')), http_content($first),
	'compression level reset after reuse');

my $slow = start_slow($t);
my $partial = '';
my $readable = IO::Select->new($slow);
while ($readable->can_read(1)) {
	my $n = sysread($slow, my $chunk, 65536);
	last if !defined $n || !$n;
	$partial .= $chunk;
	last if $partial =~ /\r\n\r\n.+/s && !$readable->can_read(0.1);
}
my ($partial_frame) = $partial =~ /\r\n\r\n(.*)/s;
ok(defined $partial_frame && length $partial_frame,
	'proxy flush sends compressed data before upstream completes');
$t->write_file('partial.zst', $partial_frame // '');
my $err = gensym;
my $pid = open3(my $stdin, my $stdout, $err, 'zstd', '-q', '-d', '-c',
	$t->testdir . '/partial.zst');
close $stdin;
my $decoded = do { local $/; <$stdout> } // '';
my $error = do { local $/; <$err> };
waitpid($pid, 0);
ok(length($decoded) && $decoded eq substr($payload, 0, length($decoded)),
	'flushed data is decodable before upstream completes');
my $busy = count_log($t, 'zstd: worker cstream busy');
is(decompress($t, request('/level1/body.html')), $payload,
	'overlapping response decodes');
ok(wait_log($t, 'zstd: worker cstream busy', $busy),
	'overlap uses separate context');
$t->write_file('resume', 'go');
is(decompress($t, $partial . http_end($slow)), $payload, 'slow response decodes');

unlink $t->testdir . '/resume' or die "remove resume: $!";
$slow = start_slow($t);
my $discard = count_log($t, 'zstd: discarded worker cstream');
close $slow or die "close slow client: $!";
ok(wait_log($t, 'zstd: discarded worker cstream', $discard),
	'aborted response discards cached context');
$t->write_file('resume', 'go');
my $after = request('/level1/body.html');
is(decompress($t, $after), $payload, 'response after abort decodes');
is(http_content($after), http_content($first), 'new context preserves level');
$before = count_log($t, 'zstd: reusing worker cstream');
is(decompress($t, request('/level1/body.html')), $payload,
	'second response after abort decodes');
ok(wait_log($t, 'zstd: reusing worker cstream', $before),
	'cache reused again after abort');

sub request {
	my ($uri) = @_;
	return http("GET $uri HTTP/1.0\r\nHost: localhost\r\n"
		. "Accept-Encoding: zstd\r\n\r\n");
}

sub start_slow {
	my ($t) = @_;
	my $before = count_log($t, 'zstd compress in');
	my $s = http_start("GET /slow HTTP/1.0\r\nHost: localhost\r\n"
		. "Accept-Encoding: zstd\r\n\r\n");
	ok(defined $s, 'slow request connected');
	ok(wait_log($t, 'zstd compress in', $before), 'slow request started compressing');
	return $s;
}

sub count_log {
	my ($t, $text) = @_;
	my $log = $t->read_file('error.log');
	return scalar(() = $log =~ /\Q$text\E/g);
}

sub wait_log {
	my ($t, $text, $previous) = @_;
	for (1 .. 100) {
		return 1 if count_log($t, $text) > $previous;
		select undef, undef, undef, 0.05;
	}
	return 0;
}

sub decompress {
	my ($t, $response) = @_;
	my $dir = $t->testdir;
	$t->write_file('cache-response.zst', http_content($response));
	system('zstd', '-q', '-d', '-f', "$dir/cache-response.zst",
		'-o', "$dir/cache-response.out") == 0
		or die "zstd decompression failed";
	return $t->read_file('cache-response.out');
}

sub upstream {
	my ($dir, $payload) = @_;
	my $server = IO::Socket::INET->new(
		Proto => 'tcp', LocalAddr => '127.0.0.1:' . port(8081),
		Listen => 5, Reuse => 1
	) or die "create upstream socket: $!";
	local $SIG{PIPE} = 'IGNORE';
	while (my $client = $server->accept()) {
		$client->autoflush(1);
		my $request = '';
		while (my $line = <$client>) {
			$request .= $line;
			last if $line =~ /^\r?\n$/;
		}
		if ($request =~ m{^GET /slow }) {
			print $client "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
				. "Content-Length: " . length($payload)
				. "\r\nConnection: close\r\n\r\n"
				. substr($payload, 0, 8192);
			for (1 .. 200) {
				last if -e "$dir/resume";
				select undef, undef, undef, 0.05;
			}
			-e "$dir/resume" or die "upstream resume timed out";
			print $client substr($payload, 8192);
		}
		close $client;
	}
}