#!/usr/bin/env perl
use strict;
use warnings;

my $file = "browser-emulator/third_party/quickjs/quickjs.cpp";
open my $fh, '<', $file or die $!;
my @lines = <$fh>;
close $fh;

my @out;
for my $i (0..$#lines) {
    my $line = $lines[$i];
    push @out, $line;

    my $next = ($i < $#lines) ? $lines[$i+1] : "";
    next if $next =~ /gc_write_barrier|GC_WRITE_BARRIER/;

    my $indent = "";
    if ($line =~ /^((?:    )*)/) { $indent = $1; }

    # Direct fast-array element assignments
    if ($line =~ /^(\s*)p_array_values\[([^\]]+)\]\s*=\s*([^;\s][^;]*);\s*$/) {
        my ($ind, $idx, $rhs) = ($1, $2, $3);
        $rhs =~ s/\s+$//;
        push @out, "${ind}GC_WRITE_BARRIER_HEAP_VALUE(&p_array_values[$idx], $rhs);\n";
        next;
    }

    # Direct property array length property assignments
    if ($line =~ /^(\s*)p_prop\[([^\]]+)\]\.u\.value\s*=\s*([^;\s][^;]*);\s*$/) {
        my ($ind, $idx, $rhs) = ($1, $2, $3);
        $rhs =~ s/\s+$//;
        push @out, "${ind}GC_WRITE_BARRIER_HEAP_VALUE(&p_prop[$idx].u.value, $rhs);\n";
        next;
    }
    if ($line =~ /^(\s*)plen->u\.value\s*=\s*([^;\s][^;]*);\s*$/) {
        my ($ind, $rhs) = ($1, $2);
        $rhs =~ s/\s+$//;
        push @out, "${ind}GC_WRITE_BARRIER_HEAP_VALUE(&plen->u.value, $rhs);\n";
        next;
    }

    # set_value calls into known heap slots
    if ($line =~ /set_value\(ctx, &([^,]+),\s*([^)]+)\);/) {
        my $slot = $1;
        my $rhs = $2;
        $rhs =~ s/^\s+|\s+$//g;
        if ($slot =~ /^(pr->u\.value|p_prop\[[^\]]+\]\.u\.value|plen->u\.value|p_array_values\[[^\]]+\]|ctx_class_proto\[[^\]]+\])$/) {
            my $kind = ($slot =~ /GCHandle/) ? "HANDLE" : "VALUE";
            # These slots all hold GCValue except GCHandle patterns? ctx_class_proto is GCValue array.
            push @out, "${indent}GC_WRITE_BARRIER_HEAP_VALUE(&$slot, $rhs);\n";
        }
    }
}

open my $out, '>', $file or die $!;
print $out @out;
close $out;
print "Done\n";
