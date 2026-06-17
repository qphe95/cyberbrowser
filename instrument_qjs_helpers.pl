#!/usr/bin/env perl
use strict;
use warnings;

my $file = "browser-emulator/third_party/quickjs/quickjs.cpp";
open my $fh, '<', $file or die $!;
my $text = do { local $/; <$fh> };
close $fh;

# Direct heap assignments -> helper calls
$text =~ s/\bpr->u\.value\s*=\s*([^;\n]+);/gc_set_heap_value(pr->u.value, $1);/g;
$text =~ s/\bpr->u\.var_ref_handle\s*=\s*([^;\n]+);/gc_set_heap_handle(pr->u.var_ref_handle, $1);/g;
$text =~ s/\bpr->u\.getset\.getter_handle\s*=\s*([^;\n]+);/gc_set_heap_handle(pr->u.getset.getter_handle, $1);/g;
$text =~ s/\bpr->u\.getset\.setter_handle\s*=\s*([^;\n]+);/gc_set_heap_handle(pr->u.getset.setter_handle, $1);/g;
$text =~ s/\bp_prop\[([^\]]+)\]\.u\.value\s*=\s*([^;\n]+);/gc_set_heap_value(p_prop[$1].u.value, $2);/g;
$text =~ s/\bplen->u\.value\s*=\s*([^;\n]+);/gc_set_heap_value(plen->u.value, $1);/g;
$text =~ s/\bp_array_values\[([^\]]+)\]\s*=\s*([^;\n]+);/gc_set_heap_value(p_array_values[$1], $2);/g;
$text =~ s/\bctx_class_proto\[([^\]]+)\]\s*=\s*([^;\n]+);/gc_set_heap_value(ctx_class_proto[$1], $2);/g;

# Also replace set_value(ctx, &known_heap_slot, rhs) where no barrier follows.
# We'll do this line-oriented to avoid messing with local-var slots.
my @lines = split /^/m, $text;
my @out;
for my $i (0..$#lines) {
    my $line = $lines[$i];
    push @out, $line;
    my $next = ($i < $#lines) ? $lines[$i+1] : "";
    if ($line =~ /set_value\(ctx, &(ctx_class_proto\[[^\]]+\]|pr->u\.value|p_prop\[[^\]]+\]\.u\.value|plen->u\.value|p_array_values\[[^\]]+\]),\s*([^)\n]+)\);\s*$/
        && $next !~ /gc_write_barrier|GC_WRITE_BARRIER/) {
        my $slot = $1;
        my $rhs = $2;
        if (!defined $rhs) { print STDERR "BAD slot='$slot' line='$line'"; next; }
        $rhs =~ s/\s+$//;
        my $indent = "";
        if ($line =~ /^((?:    )*)/) { $indent = $1; }
        push @out, "${indent}GC_WRITE_BARRIER_HEAP_VALUE(&$slot, $rhs);\n";
    }
}

open my $out, '>', $file or die $!;
print $out @out;
close $out;
print "Done\n";
