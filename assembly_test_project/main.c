
#include <stdio.h>

extern int myadd(int attr1, int attr2);



int main() {
		
	int a=4;
	int b=5;
	
	printf("Adding %d and %d and results in %d\n", a, b, myadd(a,b));

	return 0;
}
