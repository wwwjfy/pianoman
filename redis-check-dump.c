/*---------------------------------------------------------------------------

 This code is taken directly from the Redis github repository here:

 https://github.com/antirez/redis/

 and modified to make it easier to hook up to other libs.

 The original code is:

 Copyright (c) 2006-2009, Salvatore Sanfilippo
 All rights reserved.

 and carries with it the following disclaimer:

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
 IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
 OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 OF THE POSSIBILITY OF SUCH DAMAGE.

---------------------------------------------------------------------------*/

#include "redis-check-dump.h"

db_stat db_stats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* data type to hold offset in file and size */
typedef struct {
    void *data;
    size_t size;
    size_t offset;
} pos;

static unsigned char level = 0;
static pos positions[16];

#define CURR_OFFSET (positions[level].offset)

/* Hold a stack of errors */
typedef struct {
    char error[16][1024];
    size_t offset[16];
    size_t level;
} errors_t;
static errors_t errors;

#define SHIFT_ERROR(provided_offset, ...) { \
    sprintf(errors.error[errors.level], __VA_ARGS__); \
    errors.offset[errors.level] = provided_offset; \
    errors.level++; \
}

/* Data type to hold opcode with optional key name an success status */
typedef struct {
    char* key;
    int type;
    char success;
} entry;

/* Global vars that are actally used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */
static double R_Zero, R_PosInf, R_NegInf, R_Nan;

/* store string types for output */
static char types[256][16];

/* Prototypes */
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);

/* Return true if 't' is a valid object type. */
int checkType(unsigned char t) {
    /* In case a new object type is added, update the following 
     * condition as necessary. */
    return
        (t >= REDIS_HASH_ZIPMAP && t <= REDIS_HASH_ZIPLIST) ||
        t <= REDIS_HASH ||
        t >= REDIS_EXPIRETIME_MS;
}

/* when number of bytes to read is negative, do a peek */
int readBytes(void *target, long num) {
    char peek = (num < 0) ? 1 : 0;
    num = (num < 0) ? -num : num;

    pos p = positions[level];
    if (p.offset + num > p.size) {
        return 0;
    } else {
        memcpy(target, (void*)((size_t)p.data + p.offset), num);
        if (!peek) positions[level].offset += num;
    }
    return 1;
}

int processHeader() {
    char buf[10] = "_________";
    int dump_version;

    if (!readBytes(buf, 9)) {
        ERROR("Cannot read header\n");
    }

    /* expect the first 5 bytes to equal REDIS */
    if (memcmp(buf,"REDIS",5) != 0) {
        ERROR("Wrong signature in header\n");
    }

    dump_version = (int)strtol(buf + 5, NULL, 10);
    if (dump_version < 1 || dump_version > 6) {
        ERROR("Unknown RDB format version: %d\n", dump_version);
    }
    return dump_version;
}

int loadType(entry *e) {
    uint32_t offset = CURR_OFFSET;

    /* this byte needs to qualify as type */
    unsigned char t;
    if (readBytes(&t, 1)) {
        if (checkType(t)) {
            e->type = t;
            return 1;
        } else {
            SHIFT_ERROR(offset, "Unknown type (0x%02x)", t);
        }
    } else {
        SHIFT_ERROR(offset, "Could not read type");
    }

    /* failure */
    return 0;
}

int peekType() {
    unsigned char t;
    if (readBytes(&t, -1) && (checkType(t)))
        return t;
    return -1;
}

/* discard time, just consume the bytes */
int processTime(int type) {
    uint32_t offset = CURR_OFFSET;
    unsigned char t[8];
    int timelen = (type == REDIS_EXPIRETIME_MS) ? 8 : 4;

    if (readBytes(t,timelen)) {
        return 1;
    } else {
        SHIFT_ERROR(offset, "Could not read time");
    }

    /* failure */
    return 0;
}

uint32_t loadLength(int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (!readBytes(buf, 1)) return REDIS_RDB_LENERR;
    type = (buf[0] & 0xC0) >> 6;
    if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len */
        return buf[0] & 0x3F;
    } else if (type == REDIS_RDB_ENCVAL) {
        /* Read a 6 bit len encoding type */
        if (isencoded) *isencoded = 1;
        return buf[0] & 0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len */
        if (!readBytes(buf+1,1)) return REDIS_RDB_LENERR;
        return ((buf[0] & 0x3F) << 8) | buf[1];
    } else {
        /* Read a 32 bit len */
        if (!readBytes(&len, 4)) return REDIS_RDB_LENERR;
        return (unsigned int)ntohl(len);
    }
}

char *loadIntegerObject(int enctype) {
    uint32_t offset = CURR_OFFSET;
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {
        uint8_t v;
        if (!readBytes(enc, 1)) return NULL;
        v = enc[0];
        val = (int8_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (!readBytes(enc, 2)) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (!readBytes(enc, 4)) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        SHIFT_ERROR(offset, "Unknown integer encoding (0x%02x)", enctype);
        return NULL;
    }

    /* convert val into string */
    char *buf;
    buf = malloc(sizeof(char) * 128);
    sprintf(buf, "%lld", val);
    return buf;
}

char* loadLzfStringObject() {
    unsigned int slen, clen;
    char *c, *s;

    if ((clen = loadLength(NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((slen = loadLength(NULL)) == REDIS_RDB_LENERR) return NULL;

    c = malloc(clen);
    if (!readBytes(c, clen)) {
        free(c);
        return NULL;
    }

    s = malloc(slen+1);
    if (lzf_decompress(c,clen,s,slen) == 0) {
        free(c); free(s);
        return NULL;
    }

    free(c);
    return s;
}

/* returns NULL when not processable, char* when valid */
char* loadStringObject() {
    uint32_t offset = CURR_OFFSET;
    int isencoded;
    uint32_t len;

    len = loadLength(&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return loadIntegerObject(len);
        case REDIS_RDB_ENC_LZF:
            return loadLzfStringObject();
        default:
            /* unknown encoding */
            SHIFT_ERROR(offset, "Unknown string encoding (0x%02x)", len);
            return NULL;
        }
    }

    if (len == REDIS_RDB_LENERR) return NULL;

    char *buf = malloc(sizeof(char) * (len+1));
    buf[len] = '\0';
    if (!readBytes(buf, len)) {
        free(buf);
        return NULL;
    }
    return buf;
}

int processStringObject(char** store) {
    unsigned long offset = CURR_OFFSET;
    char *key = loadStringObject();
    if (key == NULL) {
        SHIFT_ERROR(offset, "Error reading string object");
        free(key);
        return 0;
    }

    if (store != NULL) {
        *store = key;
    } else {
        free(key);
    }
    return 1;
}

double* loadDoubleValue() {
    char buf[256];
    unsigned char len;
    double* val;

    if (!readBytes(&len,1)) return NULL;

    val = malloc(sizeof(double));
    switch(len) {
    case 255: *val = R_NegInf;  return val;
    case 254: *val = R_PosInf;  return val;
    case 253: *val = R_Nan;     return val;
    default:
        if (!readBytes(buf, len)) {
            free(val);
            return NULL;
        }
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return val;
    }
}

int processDoubleValue(double** store) {
    unsigned long offset = CURR_OFFSET;
    double *val = loadDoubleValue();
    if (val == NULL) {
        SHIFT_ERROR(offset, "Error reading double value");
        free(val);
        return 0;
    }

    if (store != NULL) {
        *store = val;
    } else {
        free(val);
    }
    return 1;
}

int keyMatch(entry *e){
	char *key = e->key;
	unsigned int type = e->type;
    int i;
    
    db_stats.total_keys++;

    for(i = 0; i < db_stats.match_count; i++){
        // Increment total key count (convenient location)
        char *match = db_stats.matches[i];

        size_t matchlength = strlen(match);
        size_t keylength = strlen(key);

        if(keylength >= matchlength){
            char comp[matchlength];
            strncpy(comp, key, matchlength);

            comp[matchlength] = '\0';

            if(strcmp(match, comp)==0){
                db_stats.match_counts[i] += 1;
            };
        }
    };
    
    switch(e->type) {
    case REDIS_STRING:
		db_stats.strings++;
		break;
    case REDIS_HASH:
    case REDIS_HASH_ZIPMAP:
    case REDIS_HASH_ZIPLIST:
		db_stats.hashes++;
		break;
    case REDIS_LIST:
    case REDIS_LIST_ZIPLIST:
		db_stats.lists++;
		break;
    case REDIS_SET:
    case REDIS_SET_INTSET:
		db_stats.sets++;
		break;
    case REDIS_ZSET:
    case REDIS_ZSET_ZIPLIST:
		db_stats.zsets++;
		break;
    }
    
    return 0;
}

int loadPair(entry *e) {
    uint32_t offset = CURR_OFFSET;
    uint32_t i;

    /* read key first */
    char *key;
    if (processStringObject(&key)) {
        e->key = key;
        /*keyMatch(e);*/
    } else {
        SHIFT_ERROR(offset, "Error reading entry key");
        return 0;
    }

    uint32_t length = 0;
    if (e->type == REDIS_LIST ||
        e->type == REDIS_SET  ||
        e->type == REDIS_ZSET ||
        e->type == REDIS_HASH) {
        if ((length = loadLength(NULL)) == REDIS_RDB_LENERR) {
            SHIFT_ERROR(offset, "Error reading %s length", types[e->type]);
            return 0;
        }
    }

    switch(e->type) {
    case REDIS_STRING:
    case REDIS_HASH_ZIPMAP:
    case REDIS_LIST_ZIPLIST:
    case REDIS_SET_INTSET:
    case REDIS_ZSET_ZIPLIST:
    case REDIS_HASH_ZIPLIST:
        if (!processStringObject(NULL)) {
            SHIFT_ERROR(offset, "Error reading entry value");
            return 0;
        }
    break;
    case REDIS_LIST:
    case REDIS_SET:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    case REDIS_ZSET:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element key at index %d (length: %d)", i, length);
                return 0;
            }
            offset = CURR_OFFSET;
            if (!processDoubleValue(NULL)) {
                SHIFT_ERROR(offset, "Error reading element value at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    case REDIS_HASH:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element key at index %d (length: %d)", i, length);
                return 0;
            }
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element value at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    default:
        SHIFT_ERROR(offset, "Type not implemented");
        return 0;
    }
    /* because we're done, we assume success */
    e->success = 1;
    return 1;
}

entry loadEntry() {
    entry e = { NULL, -1, 0 };
    uint32_t length, offset[4];

    /* reset error container */
    errors.level = 0;

    offset[0] = CURR_OFFSET;
    if (!loadType(&e)) {
        return e;
    }

    offset[1] = CURR_OFFSET;
    if (e.type == REDIS_SELECTDB) {
        if ((length = loadLength(NULL)) == REDIS_RDB_LENERR) {
            SHIFT_ERROR(offset[1], "Error reading database number");
            return e;
        }
        if (length > 63) {
            SHIFT_ERROR(offset[1], "Database number out of range (%d)", length);
            return e;
        }
    } else if (e.type == REDIS_EOF) {
        if (positions[level].offset < positions[level].size) {
            SHIFT_ERROR(offset[0], "Unexpected EOF");
        } else {
            e.success = 1;
        }
        return e;
    } else {
        /* optionally consume expire */
        if (e.type == REDIS_EXPIRETIME || 
            e.type == REDIS_EXPIRETIME_MS) {
            if (!processTime(e.type)) return e;
            if (!loadType(&e)) return e;
        }

        offset[1] = CURR_OFFSET;
        if (!loadPair(&e)) {
            SHIFT_ERROR(offset[1], "Error for type %s", types[e.type]);
            return e;
        }
    }

    /* all entries are followed by a valid type:
     * e.g. a new entry, SELECTDB, EXPIRE, EOF */
    offset[2] = CURR_OFFSET;
    if (peekType() == -1) {
        SHIFT_ERROR(offset[2], "Followed by invalid type");
        SHIFT_ERROR(offset[0], "Error for type %s", types[e.type]);
        e.success = 0;
    } else {
        e.success = 1;
    }

    return e;
}

void printCentered(int indent, int width, char* body) {
    char head[256], tail[256];
    memset(head, '\0', 256);
    memset(tail, '\0', 256);

    memset(head, '=', indent);
    memset(tail, '=', width - 2 - indent - strlen(body));
    printf("%s %s %s\n", head, body, tail);
}

void printValid(uint64_t ops, uint64_t bytes) {
    char body[80];
    sprintf(body, "Processed %llu valid opcodes (in %llu bytes)",
        (unsigned long long) ops, (unsigned long long) bytes);
    printCentered(4, 80, body);
}

void printSkipped(uint64_t bytes, uint64_t offset) {
    char body[80];
    sprintf(body, "Skipped %llu bytes (resuming at 0x%08llx)",
        (unsigned long long) bytes, (unsigned long long) offset);
    printCentered(4, 80, body);
}

void printErrorStack(entry *e) {
    unsigned int i;
    char body[64];

    if (e->type == -1) {
        sprintf(body, "Error trace");
    } else if (e->type >= 253) {
        sprintf(body, "Error trace (%s)", types[e->type]);
    } else if (!e->key) {
        sprintf(body, "Error trace (%s: (unknown))", types[e->type]);
    } else {
        char tmp[41];
        strncpy(tmp, e->key, 40);

        /* display truncation at the last 3 chars */
        if (strlen(e->key) > 40) {
            memset(&tmp[37], '.', 3);
        }

        /* display unprintable characters as ? */
        for (i = 0; i < strlen(tmp); i++) {
            if (tmp[i] <= 32) tmp[i] = '?';
        }
        sprintf(body, "Error trace (%s: %s)", types[e->type], tmp);
    }

    printCentered(4, 80, body);

    /* display error stack */
    for (i = 0; i < errors.level; i++) {
        printf("0x%08lx - %s\n",
            (unsigned long) errors.offset[i], errors.error[i]);
    }
}

void process() {
    uint64_t num_errors = 0, num_valid_ops = 0, num_valid_bytes = 0;
    entry entry;
    int dump_version = processHeader();

    /* Exclude the final checksum for RDB >= 5. Will be checked at the end. */
    if (dump_version >= 5) {
        if (positions[0].size < 8) {
            printf("RDB version >= 5 but no room for checksum.\n");
            exit(1);
        }
        positions[0].size -= 8;;
    }

    level = 1;
    while(positions[0].offset < positions[0].size) {
        positions[1] = positions[0];

        entry = loadEntry();
        if (!entry.success) {
            printValid(num_valid_ops, num_valid_bytes);
            printErrorStack(&entry);
            num_errors++;
            num_valid_ops = 0;
            num_valid_bytes = 0;

            /* search for next valid entry */
            uint64_t offset = positions[0].offset + 1;
            int i = 0;

            while (!entry.success && offset < positions[0].size) {
                positions[1].offset = offset;

                /* find 3 consecutive valid entries */
                for (i = 0; i < 3; i++) {
                    entry = loadEntry();
                    if (!entry.success) break;
                }
                /* check if we found 3 consecutive valid entries */
                if (i < 3) {
                    offset++;
                }
            }

            /* print how many bytes we have skipped to find a new valid opcode */
            if (offset < positions[0].size) {
                printSkipped(offset - positions[0].offset, offset);
            }

            positions[0].offset = offset;
        } else {
            num_valid_ops++;
            num_valid_bytes += positions[1].offset - positions[0].offset;

            /* advance position */
            positions[0] = positions[1];
        }
        free(entry.key);
    }

    /* because there is another potential error,
     * print how many valid ops we have processed */
    /*printValid(num_valid_ops, num_valid_bytes);*/
    db_stats.num_valid_ops = num_valid_ops;
    db_stats.num_valid_bytes = num_valid_bytes;

    /* expect an eof */
    if (entry.type != REDIS_EOF) {
        /* last byte should be EOF, add error */
        errors.level = 0;
        SHIFT_ERROR(positions[0].offset, "Expected EOF, got %s", types[entry.type]);

        /* this is an EOF error so reset type */
        entry.type = -1;
        printErrorStack(&entry);

        num_errors++;
    }

    /* Verify checksum */
    if (dump_version >= 5) {
        uint64_t crc = crc64(0,positions[0].data,positions[0].size);
        uint64_t crc2;
        unsigned char *p = (unsigned char*)positions[0].data+positions[0].size;
        crc2 = ((uint64_t)p[0] << 0) |
               ((uint64_t)p[1] << 8) |
               ((uint64_t)p[2] << 16) |
               ((uint64_t)p[3] << 24) |
               ((uint64_t)p[4] << 32) |
               ((uint64_t)p[5] << 40) |
               ((uint64_t)p[6] << 48) |
               ((uint64_t)p[7] << 56);
        if (crc != crc2) {
            SHIFT_ERROR(positions[0].offset, "RDB CRC64 does not match.");
        } else {
            printf("CRC64 checksum is OK\n");
        }
    }

    /* print summary on errors */
    if (num_errors) {
        printf("\n");
        printf("Total unprocessable opcodes: %llu\n",
            (unsigned long long) num_errors);
    }
}

void processDumpFile(int argc, char **argv){
    char *filename = argv[1];
    int fd;
    off_t size;
    struct stat stat;
    void *data;

    fd = open(filename, O_RDONLY);
    if (fd < 1) {
        ERROR("Cannot open file: %s\n", filename);
    }
    if (fstat(fd, &stat) == -1) {
        ERROR("Cannot stat: %s\n", filename);
    } else {
        size = stat.st_size;
    }

    if (sizeof(size_t) == sizeof(int32_t) && size >= INT_MAX) {
        ERROR("Cannot check dump files >2GB on a 32-bit platform\n");
    }

    data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ERROR("Cannot mmap: %s\n", filename);
    }

    /* Initialize static vars */
    positions[0].data = data;
    positions[0].size = size;
    positions[0].offset = 0;
    errors.level = 0;

    /* Object types */
    sprintf(types[REDIS_STRING], "STRING");
    sprintf(types[REDIS_LIST], "LIST");
    sprintf(types[REDIS_SET], "SET");
    sprintf(types[REDIS_ZSET], "ZSET");
    sprintf(types[REDIS_HASH], "HASH");

    /* Object types only used for dumping to disk */
    sprintf(types[REDIS_EXPIRETIME], "EXPIRETIME");
    sprintf(types[REDIS_SELECTDB], "SELECTDB");
    sprintf(types[REDIS_EOF], "EOF");

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    if (argc > 2){
        int i = 2;
        int matchc = 0;
        while ((argv[i]) && (i < MAX_MATCH_KEYS + 2)){
            db_stats.matches[matchc] = argv[i];
            db_stats.match_counts[matchc] = 0;
            matchc++;
            i++;
        }
        if ((i >= MAX_MATCH_KEYS + 2) && (argv[i])) {
            printf("Warning too many arguments; ignoring keys: ");
            while (argv[i]) {
                printf("%s ", argv[i++]);
            }
            printf("\n");
        }
        db_stats.match_count = matchc;
    }

    process();

    munmap(data, size);
    close(fd);
}

void printDbStats(){
    float ftotal = (float)db_stats.total_keys;
    printf("\nData:\n\n");
    printf("Valid Ops: %llu\n",
        (unsigned long long)db_stats.num_valid_ops);
    printf("Valid Bytes: %llu\n",
        (unsigned long long)db_stats.num_valid_bytes);

    printf("\nKey Space:\n\n");
    printf("Strings: %i (%.2f%%)\n", db_stats.strings,
        ((float)db_stats.strings/ftotal*100.00));
    printf("Lists: %i (%.2f%%)\n", db_stats.lists,
        ((float)db_stats.lists/ftotal*100.00));
    printf("Sets: %i (%.2f%%)\n", db_stats.sets,
        ((float)db_stats.sets/ftotal*100.00));
    printf("Zsets: %i (%.2f%%)\n", db_stats.zsets,
        ((float)db_stats.zsets/ftotal*100.00));
    printf("Hashes: %i (%.2f%%)\n", db_stats.hashes,
        ((float)db_stats.hashes/ftotal*100.00));
    printf("Total Keys: %i\n", db_stats.total_keys);
    printf("Total Expires: %i (%.2f%%)\n", db_stats.total_expires,
        ((float)db_stats.total_expires/ftotal*100.00));

    printf("\nMatch Stats:\n\n");
    int i;
    for(i = 0; i < db_stats.match_count; i++){
        float fmc = (float)db_stats.match_counts[i];
        printf("%i) %s %i (%.2f%%)\n", i, db_stats.matches[i],
            (int)fmc, ((fmc/ftotal)*100.00));
    }
}
