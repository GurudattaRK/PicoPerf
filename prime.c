#include<stdio.h>
#include "time.h"
int primecheck(unsigned long long int n)
{
	for(unsigned long long int i=2;i*i<=n;i++)
	{
		if(n%i==0)
		{
			return 0;
		}
	}
	return 1;
}
int main()
{
	unsigned long long int x,y,i;
	printf("enter a number(<20 digits):");   //19 or 20 digits max
	scanf("%llu",&x);
	y=18446744073709551615UL;     //20 digits

	start_measuring();
	
	printf("next nearest prime number: ");
    
    
	if(primecheck(x))
	{
		x++;
	}
	for(i=x;i<y;i++)
	{
		if(primecheck(i))
		{
			printf("%llu\n",i);
			break;
		}
	}
    
    print_measured_results(stop_measuring());
	return 0;
}