#!/usr/bin/env perl
use strict;
use warnings;

my $file = "cyberbrowser/third_party/quickjs/quickjs_handle_classes.h";
open my $fh, '<', $file or die $!;
my @lines = <$fh>;
close $fh;

my @out;
my $state = 0; # 0=normal, 1=collecting signature, 2=in body
my $sig_buf = "";
my $setter_type = "";
my $setter_param = "";
my $brace_depth = 0;

sub parse_sig {
    my ($s) = @_;
    if ($s =~ /void\s+set_[a-zA-Z0-9_]+\s*\(\s*(const\s+)?(GCValue|GCHandle)(?:\s*&\s*|\s+)([a-zA-Z0-9_]+)/s) {
        return ($2, $3);
    }
    return ("", "");
}

for my $line (@lines) {
    my $out_line = $line;

    if ($state == 0) {
        if ($line =~ /^\s*void\s+set_[a-zA-Z0-9_]+\s*\(/) {
            $sig_buf = $line;
            my $open = ($line =~ tr/(/(/);
            my $close = ($line =~ tr/)/)/);
            if ($open == $close) {
                ($setter_type, $setter_param) = parse_sig($sig_buf);
                if ($setter_type) {
                    print STDERR "ACTIVATE setter_type=$setter_type param=$setter_param\n";
                    $state = 2;
                    $brace_depth = ($sig_buf =~ tr/{/{/) - ($sig_buf =~ tr/}/}/);
                }
                $sig_buf = "";
            } else {
                $state = 1;
            }
        }
    } elsif ($state == 1) {
        $sig_buf .= $line;
        my $open = ($sig_buf =~ tr/(/(/);
        my $close = ($sig_buf =~ tr/)/)/);
        if ($open == $close) {
            ($setter_type, $setter_param) = parse_sig($sig_buf);
            if ($setter_type) {
                print STDERR "ACTIVATE setter_type=$setter_type param=$setter_param\n";
                $state = 2;
                $brace_depth = ($sig_buf =~ tr/{/{/) - ($sig_buf =~ tr/}/}/);
            } else {
                $state = 0;
            }
            $sig_buf = "";
        }
    } else {
        # in setter body
        my $open = ($line =~ tr/{/{/);
        my $close = ($line =~ tr/}/}/);
        $brace_depth += $open - $close;
        if ($line =~ /if \(p\)/) {
            print STDERR "IFP line='$line' depth=$brace_depth param='$setter_param'\n";
        }

        if ($line =~ /^\s*if \(p\) p->([a-zA-Z0-9_.\[\]]+)\s*=\s*\Q$setter_param\E\s*;\s*$/) {
            print STDERR "MATCH $setter_type $setter_param field=$1 depth=$brace_depth open=$open close=$close\n";
        }
        if ($line =~ /^\s*if \(p\) p->([a-zA-Z0-9_.\[\]]+)\s*=\s*\Q$setter_param\E\s*;\s*$/ && $open == 0 && $close == 0 && $brace_depth == 1) {
            print STDERR "EXPAND $setter_type $setter_param field=$1\n";
            my $field = $1;
            my $indent = $line =~ /^((?:    )*)/ ? $1 : "";
            my $barrier;
            if ($setter_type eq "GCHandle") {
                $barrier = "gc_write_barrier_for_heap_slot(&p->$field, $setter_param);";
            } else {
                $barrier = "gc_write_barrier_for_heap_slot(&p->$field, GC_VALUE_GET_HANDLE($setter_param));";
            }
            $out_line = "${indent}if (p) {\n${indent}    p->$field = $setter_param;\n${indent}    $barrier\n${indent}}\n";
            print STDERR "NEWLINE $out_line";
        }

        if ($brace_depth <= 0) {
            print STDERR "CLOSE setter depth=$brace_depth line='$line' open=$open close=$close";
            $state = 0;
            $setter_type = "";
            $setter_param = "";
        }
    }

    push @out, $out_line;
}

open my $out, '>', $file or die $!;
print $out @out;
close $out;
print "Done\n";
