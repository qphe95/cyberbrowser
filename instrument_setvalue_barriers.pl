#!/usr/bin/env perl
use strict;
use warnings;

my $file = "cyberbrowser/third_party/quickjs/quickjs.cpp";
open my $fh, '<', $file or die $!;
my @lines = <$fh>;
close $fh;

my @out;
for my $i (0..$#lines) {
    my $line = $lines[$i];
    push @out, $line;
    my $next = ($i < $#lines) ? $lines[$i+1] : "";
    next if $next =~ /gc_write_barrier|GC_WRITE_BARRIER/;
    if ($line =~ /set_value\(ctx, &((?:ctx_class_proto|p_prop|p_array_values)\[[^\]]+\]|pr->u\.value|plen->u\.value),\s*(.+)\);\s*$/) {
        my $slot = $1;
        my $rhs = $2;
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
