// PC build of the card builder alone (pitrex_card.cpp), for the card tests: card_cli [--if-needed | --check]
// behaves as "main-pitrex --make-card [--if-needed | --check]".
#include <stdarg.h>
#include <stdio.h>
#include "../pitrex_card.h"
#include "../pitrex_lib.h"

void pitrex_log(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
}

int main(int argc, char *argv[])
{
	return pitrex_card_main(argv[0], argc > 1 ? argv[1] : "");
}
