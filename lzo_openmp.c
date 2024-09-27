#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>

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
    lzo_bytep out ;
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
    int c;
    if (thread_count == 0){ // if thread count is not specified, use the specified number of threads
        thread_count = 8;
    }
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
    fwrite(&thread_count, 1, 1, outfile); // write the thread count to the file
    fseek(infile, 0, SEEK_END); // get the length of the file
    lzo_uint in_len = ftell(infile);
    rewind(infile);
    lzo_uint data_c_size = in_len / thread_count; // divide the file into equal chunks
    lzo_uint comp_c_size = data_c_size + data_c_size / 16 + 64 + 3; // calculate required memory for the compressed chunks
    fwrite(&data_c_size, sizeof(unsigned long int), 1, outfile); // write the size of the chunks to the file
    char complimentary = in_len % thread_count; // calculate the complimentary bytes to be added to the last chunk
    fwrite(&complimentary, sizeof(char), 1, outfile);
    struct arg_s args[thread_count]; // create an array of structs to hold the data to be processed
    for (c = 0; c < thread_count; c++){
        if (c == thread_count - 1){ // if the last chunk, add the complimentary bytes to the size of the chunk
            data_c_size += complimentary;
        }
        args[c].data = (lzo_bytep) xmalloc(data_c_size); // allocate memory for the chunk
        args[c].out = (lzo_bytep) xmalloc(comp_c_size); // allocate memory for the compressed chunk
        args[c].data_size = data_c_size;
        args[c].out_size = comp_c_size;
        fread(args[c].data, args[c].data_size, 1, infile); // read the chunk from the file
    }
    start = clock();
    {
        #pragma omp parallel for
        for (c = 0; c < thread_count; c++) {
            wrkmem = (lzo_voidp) xmalloc(LZO1X_1_MEM_COMPRESS); // allocate memory for the compression
            int r = lzo1x_1_compress(args[c].data, args[c].data_size, args[c].out, &args[c].out_size, wrkmem);
            if (r != LZO_E_OK){
                printf("parallel comp error %d\n", r);
            }
        }
    }
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    FILE *temp = tmpfile();
    unsigned int sizes[thread_count];
    for (c = 0; c < thread_count; c++){ // join the threads and write the compressed chunks to a temporary file to calculate the size of the compressed file
        fwrite(args[c].out, 1, args[c].out_size, temp);
        sizes[c] = ftell(temp);
        rewind(temp);
    }
    fclose(temp);
    fwrite(sizes, sizeof(unsigned int), thread_count, outfile); // write the sizes of the compressed chunks to the file
    for (c = 0; c < thread_count; c++){ // write the compressed chunks to the file
        fwrite(args[c].out, args[c].out_size, 1, outfile);
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
    FILE *infile = fopen(filename, "rb");
    char *ext = (char *) getExt(filename); // get the extension of the file
    char *outfilename;
    int cmp = strcmp(ext, ""); // if the file has no extension, add _dp to the end of the filename to indicate that the file is decompressed using parallel decompression
    if (cmp == 0){
        outfilename = (char *) xmalloc(54);
        strcpy(outfilename, filename);
        strcat(outfilename, "_dp");
    } else if (cmp > 0){
        outfilename = (char *) xmalloc(54);
        strncpy(outfilename, filename, strlen(filename) - strlen(ext));
        outfilename[strlen(filename) - strlen(ext)] = '\0';
        strcat(outfilename, "_dp");
    }
    FILE *outfile = fopen(outfilename, "wb");
    fread(&thread_count, sizeof(char), 1, infile); // read the thread count from the file
    lzo_uint data_c_size;
    fread(&data_c_size, sizeof(unsigned long int), 1, infile); // read the size of the chunks from the file
    char complimentary;
    fread(&complimentary, sizeof(char), 1, infile); // read the complimentary bytes from the file
    lzo_uint in_len = 0;
    lzo_uint out_len = 0;
    int c;
    unsigned int compsizes[thread_count];
    for (c = 0; c < thread_count; c++){ // join the threads and write the compressed chunks to a temporary file to calculate the size of the compressed file
        unsigned int *temp_var = 0;
        fread(&temp_var, sizeof(unsigned int), 1, infile);
        compsizes[c] = (unsigned int) temp_var;
    }
    // create an array of structs to hold the data to be processed
    struct arg_s args[thread_count];
    struct arg_s testargs[thread_count];
    for (c = 0; c < thread_count; c++){
        if (c == thread_count - 1){ // if the last chunk, add the complimentary bytes to the size of the chunk
            data_c_size += complimentary;
        }
        args[c].data = (lzo_bytep) xmalloc(compsizes[c]); // allocate memory for the chunk
        args[c].out = (lzo_bytep) xmalloc(data_c_size); // allocate memory for the decompressed chunk
        args[c].data_size = compsizes[c];
        args[c].out_size = data_c_size;
        fread(args[c].data, 1, args[c].data_size, infile); // read the compressed chunk from the file
        testargs[c].data = (lzo_bytep) xmalloc(compsizes[c]); // allocate memory for the chunk
        testargs[c].out = (lzo_bytep) xmalloc(data_c_size); // allocate memory for the decompressed chunk
        testargs[c].data_size = compsizes[c];
        testargs[c].out_size = data_c_size;
        memcpy(testargs[c].data, args[c].data, args[c].data_size);
        in_len += args[c].data_size;
    }
    start = clock();
    {
    #pragma omp parallel for
        for (c = 0; c < thread_count; c++) {
            int r = lzo1x_decompress(args[c].data, args[c].data_size, args[c].out, &args[c].out_size, NULL);
            if (r != LZO_E_OK){
               printf("parallel decomp error %d\n", r);
           }
        }
    }
    result.time = ((double) (clock() - start)) / CLOCKS_PER_SEC;
    for (c = 0; c < thread_count; c++){
        fwrite(args[c].out, args[c].out_size, 1, outfile); // write the decompressed chunks to the file
        out_len += args[c].out_size;
    }
    fclose(infile);
    fclose(outfile);
    result.in_size = in_len;
    result.out_size = out_len;
    result.ratio = (double) result.out_size / result.in_size;
    free(outfilename);
    return result;
}

//serial compression function
struct result_s compress_data_serial(char filename[]){
    struct result_s result;
    clock_t start;
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
    wrkmem = (lzo_voidp) xmalloc(LZO1X_1_MEM_COMPRESS); // allocate memory for the compression
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
bool test_file_integrity(char filename[]){
    FILE *original_file = fopen(filename, "rb");
    char *decompressed_filename = (char *) xmalloc(strlen(filename) + 4);
    strcpy(decompressed_filename, filename);
    strcat(decompressed_filename, "_dp");
    FILE *decompressed_file = fopen(decompressed_filename, "rb");
    fseek(original_file, 0, SEEK_END);
    long int original_size = ftell(original_file);
    fseek(decompressed_file, 0, SEEK_END);
    long int decompressed_size = ftell(decompressed_file);
    if (original_size != decompressed_size){
        printf("file sizes do not match - %s and %s\n", filename, decompressed_filename);
        return false;
    }
    rewind(original_file);
    rewind(decompressed_file);
    // compare byte by byte
    char original_byte;
    char decompressed_byte;
    while (fread(&original_byte, 1, 1, original_file) == 1){
        fread(&decompressed_byte, 1, 1, decompressed_file);
        if (original_byte != decompressed_byte){
            printf("file contents do not match - %s and %s\n", filename, decompressed_filename);
            return false;
        }
    }
    fclose(original_file);
    fclose(decompressed_file);
    return true;
}
//main function to run the tests
int main(){
    //checks if the lzo can be initialized
    if (lzo_init() != LZO_E_OK){
        printf("lzo init failed\n");
        return 1;
    }
    printf("Parallel LZO compression & decompression test using Calgary Corpus and some other big files\n");
    char filenames[22][20] = {"trans","paper4","paper3","news","paper2","book1","geo","obj2","paper1","progp","paper5","pic","paper6","progc","progl","bib","obj1","book2", "big1", "big4", "book3", "html"};
    struct result_s results_s[22][2]; // 0 for compression, 1 for decompression
    struct result_s results_p[22][2]; // 0 for compression, 1 for decompression
    for (int tempcount = 0; tempcount < 22; tempcount++){
        results_s[tempcount][0].in_size = 0;
        results_s[tempcount][0].out_size = 0;
        results_s[tempcount][0].ratio = 0;
        results_s[tempcount][0].time = 0;
        results_s[tempcount][1].in_size = 0;
        results_s[tempcount][1].out_size = 0;
        results_s[tempcount][1].ratio = 0;
        results_s[tempcount][1].time = 0;
        results_p[tempcount][0].in_size = 0;
        results_p[tempcount][0].out_size = 0;
        results_p[tempcount][0].ratio = 0;
        results_p[tempcount][0].time = 0;
        results_p[tempcount][1].in_size = 0;
        results_p[tempcount][1].out_size = 0;
        results_p[tempcount][1].ratio = 0;
        results_p[tempcount][1].time = 0;
    }
    int j;
    double sumtime = 0;
    double sumratio = 0;
    for (j = 0; j < 22; j++) { // for each file in the corpus do the following
        sumtime = 0;
        sumratio = 0;
        char *temp;
        printf("file is %s\n", filenames[j]);
        for (int i = 0; i < 10; i++){
            results_s[j][0] = compress_data_serial(filenames[j]); // compress the file using serial compression
            sumtime += results_s[j][0].time;
            sumratio += results_s[j][0].ratio;
        }
        results_s[j][0].time = sumtime / 10;
        results_s[j][0].ratio = sumratio / 10;
        sumratio = 0;
        sumtime = 0;
        printf("s comp %d done\n", j);
        temp = (char *) xmalloc(strlen(filenames[j]) + 4);
        strcpy(temp, filenames[j]);
        strcat(temp, ".lzo");
        for (int i = 0; i < 10; i++){
            results_s[j][1] = decompress_data_serial(temp); // decompress the file using serial decompression
            sumtime += results_s[j][1].time;
            sumratio += results_s[j][1].ratio;
        }
        results_s[j][1].time = sumtime / 10;
        results_s[j][1].ratio = sumratio / 10;
        sumratio = 0;
        sumtime = 0;
        printf("s decomp %d done\n", j);
        free(temp);
        for (int i = 0; i < 10; i++){
            results_p[j][0] = compress_data_parallel(filenames[j]); // compress the file using parallel compression
            sumtime += results_p[j][0].time;
            sumratio += results_p[j][0].ratio;
        }
        results_p[j][0].time = sumtime / 10;
        printf("sumtime is %f --- ", sumtime);
        printf("mean time is %f\n", results_p[j][0].time);
        results_p[j][0].ratio = sumratio / 10;
        sumratio = 0;
        sumtime = 0;
        printf("p comp %d done\n", j);
        temp = (char *) xmalloc(strlen(filenames[j]) + 5);
        strcpy(temp, filenames[j]);
        strcat(temp, ".plzo");
        for (int i = 0; i < 10; i++){
            results_p[j][1] = decompress_data_parallel(temp); // decompress the file using parallel decompression
            sumtime += results_p[j][1].time;
            sumratio += results_p[j][1].ratio;
        }
        results_p[j][1].time = sumtime / 10;
        results_p[j][1].ratio = sumratio / 10;
        printf("p decomp %d done\n", j);
        free(temp);
        if (test_file_integrity(filenames[j]) == false){
            printf("file integrity test failed at file %d - %s\n", j, filenames[j]);
        }
    }
    char results_filename[100] = "results_omp_actual_";
    // concat the thread count to the filename
    char thread_count_str[2];
    sprintf(thread_count_str, "%d", (int) thread_count);
    strcat(results_filename, thread_count_str);
    strcat(results_filename, "_threads.txt");
    FILE *results_file = fopen(results_filename, "wb");
    fprintf(results_file, "%-30s\t%15s\t%15s\t%15s\t%15s\t%15s\t%15s\t%15s\n\n", "filename", "file-size", "comp-size", "comp-ratio", "comp-time", "decomp-size", "decomp-ratio", "decomp-time");
    for (j = 0; j < 22; j++){
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
    fprintf(results_file, "%-15s%15d\n", "thread-count:", (int) thread_count);
    printf("done\n");
    return 0;
}