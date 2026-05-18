#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"

int main(void) {
	printf("Content-Type: application/json\r\n\r\n");

	FILE *fp = fopen(TEMP_PATH, "r");
	if (fp == NULL) {
		printf("{\"status\": \"Error\", \"msg\": \"Nao foi possivel abrir o arquivo de temperatura\"}\n");
		return 1;
	}

	char buffer[64];
	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		fclose(fp);
		printf("{\"status\": \"Error\", \"msg\": \"Nao foi possivel ler a temperatura\"}\n");
		return 1;
	}

	fclose(fp);

	errno = 0;
	char *endptr = NULL;
	long temp_millicelsius = strtol(buffer, &endptr, 10);
	if (errno != 0 || endptr == buffer) {
		printf("{\"status\": \"Error\", \"msg\": \"Formato invalido da temperatura\"}\n");
		return 1;
	}

	double temp_celsius = (double)temp_millicelsius / 1000.0;

	printf("{\"status\": \"Success\", \"data\": {\"raw\": %ld, \"celsius\": %.1f}}\n",
		   temp_millicelsius,
		   temp_celsius);

	return 0;
}
