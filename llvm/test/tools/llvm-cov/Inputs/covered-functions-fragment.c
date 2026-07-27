#include "covered-functions-fragment.h"

int covered(int x) { return DOUBLE_POSITIVE(x); }

int main(void) { return covered(1) == 2 ? 0 : 1; }
