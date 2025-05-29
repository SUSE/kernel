#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Generate the x86_cap/bug_flags[] arrays from include/asm/cpufeatures.h
#

set -e

OUT=$1

dump_array()
{
	ARRAY=$1
	SIZE=$2
	PFX=$3
	POSTFIX=$4
	IN=$5

	PFX_SZ=$(echo $PFX | wc -c)
	TABS="$(printf '\t\t\t\t\t')"

	echo "const char * const $ARRAY[$SIZE] = {"

	# Iterate through any input lines starting with #define $PFX
	sed -n -e 's/\t/ /g' -e "s/^ *# *define *$PFX//p" $IN |
	while read i
	do
		# Name is everything up to the first whitespace
		NAME="$(echo "$i" | sed 's/ .*//')"

		# If the /* comment */ starts with a quote string, grab that.
		VALUE="$(echo "$i" | sed -n 's@.*/\* *\("[^"]*"\).*\*/@\1@p')"
		[ -z "$VALUE" ] && VALUE="\"$NAME\""
		[ "$VALUE" = '""' ] && continue

		# Name is uppercase, VALUE is all lowercase
		VALUE="$(echo "$VALUE" | tr A-Z a-z)"

		T=$(( $PFX_SZ + $(echo $POSTFIX | wc -c) + 2 ))
		TABS="$(printf '\t\t\t\t\t\t\t')"
		TABCOUNT=$(( ( 7*8 - $T - $(echo "$NAME" | wc -c) ) / 8 ))
		printf "\t[%s(%s)]%.*s = %s,\n" "$POSTFIX" "$PFX$NAME" "$TABCOUNT" "$TABS" "$VALUE"
	done
	echo "};"
}

trap 'rm "$OUT"' EXIT

(
	echo "#ifndef _ASM_X86_CPUFEATURES_H"
	echo "#include <asm/cpufeatures.h>"
	echo "#endif"
	echo ""

	dump_array "x86_cap_flags" "(NCAPINTS+NEXTCAPINTS)*32" "X86_FEATURE_" "X86_FEATUREINDEX" $2
	echo ""

	dump_array "x86_bug_flags" "(NBUGINTS+NEXTBUGINTS)*32" "X86_BUG_" "X86_BUGINDEX" $2
	echo ""

	echo "#ifdef CONFIG_X86_VMX_FEATURE_NAMES"
	echo "#ifndef _ASM_X86_VMXFEATURES_H"
	echo "#include <asm/vmxfeatures.h>"
	echo "#endif"
	dump_array "x86_vmx_flags" "NVMXINTS*32" "VMX_FEATURE_" "" $3
	echo "#endif /* CONFIG_X86_VMX_FEATURE_NAMES */"
) > $OUT

trap - EXIT
