#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include <time.h>
#include "math.h"

//required configuration
static const char *progname = NULL;
#define WANT_LZO_MALLOC 1
#define WANT_XMALLOC 1
#include "portab.h"
lzo_voidp wrkmem;

//1 byte var to hold thread count
char static thread_count = 0;
//struct to hold the data to be processed
struct arg_s {
    lzo_bytep data;
    lzo_bytep out;
    lzo_uint data_size;
    lzo_uint out_size;
};
//struct to hold the results
struct result_s {
    lzo_uint in_size;
    lzo_uint out_size;
    double ratio;
    double time;
};
//compression function to be used during parallel compression
void *compress(void *arg) {
    struct arg_s *args = (struct arg_s *) arg;
    int r = lzo1x_1_compress(args->data, args->data_size, args->out, &args->out_size, wrkmem);
    if (r != LZO_E_OK){
        printf("parallel comp error %d\n", r);
    }
}
//decompression function to be used during parallel decompression
void *decompress(void *arg) {
    struct arg_s *args = (struct arg_s *) arg;
    int r = lzo1x_decompress(args->data, args->data_size, args->out, &args->out_size, NULL);
    if (r != LZO_E_OK){
        printf("parallel decomp error %d\n", r);
    }
}
//function to get the extension of a file
const char *getExt (const char *fspec) {
    char *e = strrchr (fspec, '.');
    if (e == NULL)
        e = "";
    return e;
}
//parallel compression function
struct result_s compress_data_parallel(char filename[]){
    struct result_s result;
    clock_t start;
    start = clock();
    int c;
    if (thread_count == 0){ // if thread count is not specified, use the specified number of threads
        thread_count = 8;
    }
    wrkmem = (lzo_voidp) xmalloc(LZO1X_1_MEM_COMPRESS); // allocate memory for the compression
    FILE *infile = fopen(filename, "rb");
    char *ext = (char *) getExt(filename); // get the extension of the file
    char *outfilename;
    int cmp = strcmp(ext, ""); // if the file has no extension, add .plzo to the end of the filename to indicate that the file is compressed using parallel compression
    if (cmp == 0){
        outfilename = (char *) xmalloc(strlen(filename) + 5);
        strcpy(outfilename, filename);
        strcat(outfilename, ".plzo");
    } else if (cmp > 0){
        outfilename = (char *) xmalloc(strlen(filename) + 1);
        strncpy(outfilename, filename, strlen(filename) - strlen(ext));
        outfilename[strlen(filename) - strlen(ext)] = '\0';
        strcat(outfilename, "plzo");
    }
    FILE *outfile = fopen(outfilename, "wb");
    fwrite(&thread_count, sizeof(char), 1, outfile); // write the thread count to the file
    fseek(infile, 0, SEEK_END); // get the length of the file
    lzo_uint in_len = ftell(infile);
    rewind(infile);
    lzo_uint data_c_size = in_len / thread_count; // divide the file into equal chunks
    lzo_uint comp_c_size = data_c_size + data_c_size / 16 + 64 + 3; // calculate required memory for the compressed chunks
    fwrite(&data_c_size, sizeof(unsigned long int), 1, outfile); // write the size of the chunks to the file
    char complimentary = in_len % thread_count; // calculate the complimentary bytes to be added to the last chunk
    fwrite(&complimentary, sizeof(char), 1, outfile);
    struct arg_s args[thread_count]; // create an array of structs to hold the data to be processed
    pthread_t threads[thread_count]; // create an array of threads to process the data
    for (c = 0; c < thread_count; c++){
        if (c == thread_count - 1){ // if the last chunk, add the complimentary bytes to the size of the chunk
            data_c_size += complimentary;
        }
        args[c].data = (lzo_bytep) xmalloc(data_c_size); // allocate memory for the chunk
        args[c].out = (lzo_bytep) xmalloc(comp_c_size); // allocate memory for the compressed chunk
        args[c].data_size = data_c_size;
        args[c].out_size = comp_c_size;
		fread(args[c].data, 1, args[c].data_size, infile); // read the chunk from the file
    }
    start = clock(); // start the timer
    for (c = 0; c < thread_count; c++){ // create threads to process the chunks and compress them
        pthread_create(&threads[c], NULL, compress, (void *) &args[c]);
    }
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    FILE *temp = tmpfile();
    unsigned int sizes[thread_count];
    for (c = 0; c < thread_count; c++){ // join the threads and write the compressed chunks to a temporary file to calculate the size of the compressed file
        pthread_join(threads[c], NULL);
        fwrite(args[c].out, 1, args[c].out_size, temp);
        sizes[c] = ftell(temp);
        rewind(temp);
    }
    fclose(temp);
    fwrite(sizes, sizeof(unsigned int), thread_count, outfile); // write the sizes of the compressed chunks to the file
    for (c = 0; c < thread_count; c++){ // write the compressed chunks to the file
        fwrite(args[c].out, 1, args[c].out_size, outfile);
        free(args[c].data);
        free(args[c].out);
    }
    result.in_size = in_len;
    result.out_size = ftell(outfile);
    result.ratio = (double) result.out_size / result.in_size;
    fclose(outfile);
    fclose(infile);
    free(wrkmem);
    return result;
}
//parallel decompression function
struct result_s decompress_data_parallel(char filename[]){
    struct result_s result;
    clock_t start;
    int c;
    if (thread_count == 0){ // if thread count is not specified, use the specified number of threads
        // !!!! SINCE THE TESTS ARE RUN ON THE SAME MACHINE, THE THREAD COUNT IS SET TO THE NUMBER OF CORES FOR THE EASE OF TESTING
        thread_count = 8;
    }
    FILE *infile = fopen(filename, "rb");
    fseek(infile, 0, SEEK_END);
    unsigned int length = ftell(infile); // get the length of the file
    rewind(infile);
    char *ext = (char *) getExt(filename);
    char *outfilename = (char *) xmalloc(strlen(filename) + 5);
    strncpy(outfilename, filename, strlen(filename) - strlen(ext));
    outfilename[strlen(filename) - strlen(ext)] = '\0';
    strcat(outfilename, "_dp"); // concatenate _dp to the end of the filename to indicate that the file is decompressed using parallel decompression
    FILE *outfile = fopen(outfilename, "wb");
    char t_count;
    fread(&t_count, sizeof(char), 1, infile);
    lzo_uint data_c_size = (lzo_uint) xmalloc(sizeof(unsigned long int)); // allocate memory for the size of the chunks
    fread(&data_c_size, sizeof data_c_size, 1, infile);
    char complimentary;
    fread(&complimentary, sizeof(char), 1, infile);
    unsigned int *comp_c_sizes[t_count];
	for (c = 0; c < thread_count; c++){
		comp_c_sizes[c] = malloc(sizeof(unsigned int));
		fread(comp_c_sizes[c], sizeof(unsigned int), 1, infile);
	}
    struct arg_s args[thread_count];
    pthread_t threads[thread_count];
    for (c = 0; c < thread_count; c++){
        if (c == thread_count - 1){
            data_c_size += complimentary;
        }
        args[c].data = (lzo_bytep) xmalloc(*comp_c_sizes[c]); // allocate memory for the compressed chunk
        args[c].out = (lzo_bytep) xmalloc(data_c_size); // allocate memory for the decompressed chunk
        args[c].data_size = *comp_c_sizes[c];
        args[c].out_size = data_c_size;
		fread(args[c].data, 1, *comp_c_sizes[c], infile); // read the compressed chunk from the file
    }
    start = clock();
    for (c = 0; c < thread_count; c++){ // create threads to process the chunks and decompress them
        pthread_create(&threads[c], NULL, decompress, (void *) &args[c]);
    }
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    fclose(infile);
    for (c = 0; c < thread_count; c++){
        pthread_join(threads[c], NULL);
        fwrite(args[c].out, 1, args[c].out_size, outfile);
        free(args[c].data);
        free(args[c].out);
    }
    result.in_size = length;
    result.out_size = ftell(outfile);
    result.ratio = (double) result.out_size / result.in_size;
    fclose(outfile);
    return result;
}
//serial compression function
struct result_s compress_data_serial(char filename[]){
    struct result_s result;
    clock_t start;
    wrkmem = (lzo_voidp) xmalloc(LZO1X_1_MEM_COMPRESS); // allocate memory for the compression
    FILE *infile = fopen(filename, "rb");
    fseek(infile, 0, SEEK_END);
    lzo_uint in_len = ftell(infile);
    rewind(infile);
    lzo_bytep data = (lzo_bytep) xmalloc(in_len);
    fread(data, 1, in_len, infile);
    fclose(infile);
    lzo_uint out_len = in_len + in_len / 16 + 64 + 3;
    lzo_bytep out = (lzo_bytep) xmalloc(out_len);
    start = clock();
    int r = lzo1x_1_compress(data, in_len, out, &out_len, wrkmem); // compress the data using serial compression
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    if (r != LZO_E_OK){
        printf("serial comp error %d\n", r);
    }
    char outfilename[strlen(filename) + 4];
    char *ext = (char *) getExt(filename);
    int cmp = strcmp(ext, "");
    if (cmp == 0){ // if the file has no extension, add .lzo to the end of the filename to indicate that the file is compressed using serial compression
        strcpy(outfilename, filename);
        strcat(outfilename, ".lzo");
    } else if (cmp > 0){
        strncpy(outfilename, filename, strlen(filename) - strlen(ext));
        outfilename[strlen(filename) - strlen(ext)] = '\0';
        strcat(outfilename, "lzo");
    }
    FILE *outfile = fopen(outfilename, "wb");
    fwrite(out, 1, out_len, outfile);
    fclose(outfile);
    free(data);
    free(out);
    result.in_size = in_len;
    result.out_size = out_len;
    result.ratio = (double) result.out_size / result.in_size;
    free(wrkmem);
    return result;
}
//serial decompression function
struct result_s decompress_data_serial(char filename[]){
    struct result_s result;
    clock_t start;
    FILE *infile = fopen(filename, "rb");
    fseek(infile, 0, SEEK_END);
    lzo_uint in_len = ftell(infile);
    rewind(infile);
    lzo_bytep data = (lzo_bytep) malloc(in_len);
    fread(data, 1, in_len, infile);
    fclose(infile);
    lzo_uint out_len = in_len * 16;
    lzo_bytep out = (lzo_bytep) malloc(out_len);
    start = clock();
    int r = lzo1x_decompress(data, in_len, out, &out_len, NULL); // decompress the data using serial decompression
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    if (r != LZO_E_OK){
        printf("serial decomp error %d\n", r);
    }
    char outfilename[strlen(filename) + 4];
    char *ext = (char *) getExt(filename);
    int cmp = strcmp(ext, "");
    if (cmp == 0){ // if the file has no extension, add _ds to the end of the filename to indicate that the file is decompressed using serial decompression
        strcpy(outfilename, filename);
        strcat(outfilename, "_ds");
    } else if (cmp > 0) {
        strncpy(outfilename, filename, strlen(filename) - strlen(ext));
        outfilename[strlen(filename) - strlen(ext)] = '\0';
        strcat(outfilename, "_ds");
    }
    FILE *outfile = fopen(outfilename, "wb");
    fwrite(out, 1, out_len, outfile);
    fclose(outfile);
    free(data);
    free(out);
    result.in_size = in_len;
    result.out_size = out_len;
    result.ratio = (double) result.out_size / result.in_size;
    return result;
}
//main function to run the tests
int main(){
    //checks if the lzo can be initialized
    if (lzo_init() != LZO_E_OK){
        printf("lzo init failed\n");
        return 1;
    }
    printf("Parallel LZO compression & decompression test using Calgary Corpus\n");
    char filenames[18][20] = {"trans","paper4","paper3","news","paper2","book1","geo","obj2","paper1","progp","paper5","pic","paper6","progc","progl","bib","obj1","book2"};
    struct result_s results_s[18][2]; // 0 for compression, 1 for decompression
    struct result_s results_p[18][2]; // 0 for compression, 1 for decompression
    int j;
    for (j = 0; j < 18; j++) { // for each file in the corpus do the following
        char *temp;
        printf("file is %s\n", filenames[j]);
        results_s[j][0] = compress_data_serial(filenames[j]); // compress the file using serial compression
        printf("s comp %d done\n", j);
        temp = (char *) xmalloc(strlen(filenames[j]) + 4);
        strcpy(temp, filenames[j]);
        strcat(temp, ".lzo");
        results_s[j][1] = decompress_data_serial(temp); // decompress the file using serial decompression
        printf("s decomp %d done\n", j);
        free(temp);
        results_p[j][0] = compress_data_parallel(filenames[j]); // compress the file using parallel compression
        printf("p comp %d done\n", j);
        temp = (char *) xmalloc(strlen(filenames[j]) + 5);
        strcpy(temp, filenames[j]);
        strcat(temp, ".plzo");
        results_p[j][1] = decompress_data_parallel(temp); // decompress the file using parallel decompression
        printf("p decomp %d done\n", j);
        free(temp);
    }
    char results_filename[100] = "results_";
    // concat the thread count to the filename
    char thread_count_str[2];
    sprintf(thread_count_str, "%d", (int) thread_count);
    strcat(results_filename, thread_count_str);
    strcat(results_filename, "_threads.txt");
    FILE *results_file = fopen(results_filename, "wb");
    fprintf(results_file, "%-30s\t%15s\t%15s\t%15s\t%15s\t%15s\t%15s\t%15s\n\n", "filename", "file size", "comp size", "comp ratio", "comp time", "decomp size", "decomp ratio", "decomp time");
    for (j = 0; j < 18; j++){
        fprintf(results_file, "%-30s\n", filenames[j]);
        fprintf(results_file, "%30s\t", "serial");
        char file_size[20];
        sprintf(file_size, "%15.4lu", results_s[j][0].in_size);
        fwrite(file_size, 1, strlen(file_size), results_file);
        fwrite("\t", 1, 1, results_file);
        char comp_size[20];
        sprintf(comp_size, "%15.4lu", results_s[j][0].out_size);
        fwrite(comp_size, 1, strlen(comp_size), results_file);
        fwrite("\t", 1, 1, results_file);
        char comp_ratio[20];
        sprintf(comp_ratio, "%15.8f", results_s[j][0].ratio);
        fwrite(comp_ratio, 1, strlen(comp_ratio), results_file);
        fwrite("\t", 1, 1, results_file);
        char comp_time[20];
        sprintf(comp_time, "%15.8f", results_s[j][0].time);
        fwrite(comp_time, 1, strlen(comp_time), results_file);
        fwrite("\t", 1, 1, results_file);
        char decomp_size[20];
        sprintf(decomp_size, "%15.4lu", results_s[j][1].out_size);
        fwrite(decomp_size, 1, strlen(decomp_size), results_file);
        fwrite("\t", 1, 1, results_file);
        char decomp_ratio[20];
        sprintf(decomp_ratio, "%15.8f", results_s[j][1].ratio);
        fwrite(decomp_ratio, 1, strlen(decomp_ratio), results_file);
        fwrite("\t", 1, 1, results_file);
        char decomp_time[20];
        sprintf(decomp_time, "%15.8f", results_s[j][1].time);
        fwrite(decomp_time, 1, strlen(decomp_time), results_file);
        fwrite("\n", 1, 1, results_file);
        fprintf(results_file, "%30s\t", "parallel");
        sprintf(file_size, "%15.4lu", results_p[j][0].in_size);
        fwrite(file_size, 1, strlen(file_size), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(comp_size, "%15.4lu", results_p[j][0].out_size);
        fwrite(comp_size, 1, strlen(comp_size), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(comp_ratio, "%15.8f", results_p[j][0].ratio);
        fwrite(comp_ratio, 1, strlen(comp_ratio), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(comp_time, "%15.8f", results_p[j][0].time);
        fwrite(comp_time, 1, strlen(comp_time), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(decomp_size, "%15.4lu", results_p[j][1].out_size);
        fwrite(decomp_size, 1, strlen(decomp_size), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(decomp_ratio, "%15.8f", results_p[j][1].ratio);
        fwrite(decomp_ratio, 1, strlen(decomp_ratio), results_file);
        fwrite("\t", 1, 1, results_file);
        sprintf(decomp_time, "%15.8f", results_p[j][1].time);
        fwrite(decomp_time, 1, strlen(decomp_time), results_file);
        fwrite("\n\n", 2, 1, results_file);
    }
	fprintf(results_file, "%-15s%15d", "thread count:", (int) thread_count);
    return 0;
}
