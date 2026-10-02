#!/usr/bin/perl
# Print every TR/TRC/TRN/N_ text in the C files given, one per line, as it is
# written between the quotes (escapes kept; adjacent literals joined; TRC as
# 场景|中文). Lines only the public build has (the marked // comments) count too.
# --trn: only the TRN texts (their English row is one|other).
# SPDX-License-Identifier: MIT
use strict; use warnings;
local $/;
my $lit = qr/"(?:[^"\\\n]|\\.)*"/;
my $trn = @ARGV && $ARGV[0] eq '--trn' ? shift : 0;
sub cat { my $s = shift; my $o = ""; $o .= $1 while $s =~ /"((?:[^"\\\n]|\\.)*)"/g; $o }
for my $f (@ARGV) {
    open my $fh, '<', $f or die "$f: $!";
    $_ = <$fh>;
    my $pub = 'PUBLIC' . '-ONLY:';   # spelled apart: tools/strip-private.pl acts on the marker itself
    s{^([ \t]*)//\s*\Q$pub\E}{$1}mg;
    s{/\*.*?\*/}{}gs;
    s{("(?:[^"\\\n]|\\.)*")|//[^\n]*}{defined $1 ? $1 : ""}ge;   # // inside strings stays
    if ($trn) { while (/\bTRN\s*\(\s*($lit(?:\s*$lit)*)/g) { print cat($1), "\n" } next }
    while (/\bTRC\s*\(\s*($lit(?:\s*$lit)*)\s*,\s*($lit(?:\s*$lit)*)/g) { print cat($1), "|", cat($2), "\n" }
    while (/\b(?:TR|TRN|N_)\s*\(\s*($lit(?:\s*$lit)*)/g) { print cat($1), "\n" }
}
