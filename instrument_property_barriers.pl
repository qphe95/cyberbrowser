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
    # Avoid double-barrier
    next if $next =~ /gc_write_barrier|GC_WRITE_BARRIER/;

    # Determine leading indentation of current line
    my $indent = "";
    if ($line =~ /^((?:    )*)/) { $indent = $1; }

    if ($line =~ /^(\s*)pr->u\.value\s*=\s*([^;\s][^;]*);\s*$/) {
        my $rhs = $2;
        # trim trailing spaces
        $rhs =~ s/\s+$//;
        push @out, "${indent}GC_WRITE_BARRIER_HEAP_VALUE(&pr->u.value, $rhs);\n";
    }
    elsif ($line =~ /^(\s*)pr->u\.getset\.getter_handle\s*=\s*([^;\s][^;]*);\s*$/) {
        my $rhs = $2; $rhs =~ s/\s+$//;
        push @out, "${indent}GC_WRITE_BARRIER_HEAP_HANDLE(&pr->u.getset.getter_handle, $rhs);\n";
    }
    elsif ($line =~ /^(\s*)pr->u\.getset\.setter_handle\s*=\s*([^;\s][^;]*);\s*$/) {
        my $rhs = $2; $rhs =~ s/\s+$//;
        push @out, "${indent}GC_WRITE_BARRIER_HEAP_HANDLE(&pr->u.getset.setter_handle, $rhs);\n";
    }
    elsif ($line =~ /^(\s*)pr->u\.var_ref_handle\s*=\s*([^;\s][^;]*);\s*$/) {
        my $rhs = $2; $rhs =~ s/\s+$//;
        push @out, "${indent}GC_WRITE_BARRIER_HEAP_HANDLE(&pr->u.var_ref_handle, $rhs);\n";
    }
}

open my $out, '>', $file or die $!;
print $out @out;
close $out;
print "Done\n";
