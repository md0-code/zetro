/* The .pok cheat file (trainers): a text file of lines, each starting with a letter. N followed by a name starts a
 * trainer; M and Z lines are its pokes, M saying more follow and Z that this is the last; Y ends the file. A poke
 * line is `L bank address value original`, numbers in decimal: the bank's bits 0 to 2 are a 128K RAM page and its
 * bit 3 (so bank 8) means "the address as the CPU sees it"; the value 256 means "ask the user"; the original is the
 * value to put back when the trainer is switched off, 0 where it is not known.
 *
 * A trainer that is damaged (a line that is not a poke, a number out of range, a trainer cut short or without pokes)
 * is kept with its name and the number of the line that spoiled it, marked unusable. A file that holds no trainer at
 * all is an error. Every length is checked and a damaged file never crashes the reader. ZetRo's own code, written
 * from the format description. */
#ifndef ZR_POK_H
#define ZR_POK_H

#include <stddef.h>
#include <stdint.h>

#define ZR_POK_NAME 47                      /* characters of a trainer's name kept */
#define ZR_POK_MAX_TRAINERS 512
#define ZR_POK_MAX_POKES 256                /* in one trainer */
#define ZR_POK_LIMIT (1u << 20)             /* the largest file read */
#define ZR_POK_ASK 256                      /* a poke's value that asks the user */
#define ZR_POK_CPU_BANK 8                   /* the bank bit that means the CPU's own addresses */

typedef struct zr_pok_poke {
    uint8_t bank;                           /* 0 to 15: bit 3 set, the CPU's addresses; else a 128K RAM page */
    uint16_t address;
    uint16_t value;                         /* 0 to 255, or ZR_POK_ASK */
    uint8_t original;                       /* 0: not known */
    unsigned line;                          /* its line in the file */
} zr_pok_poke;

typedef struct zr_pok_trainer {
    char name[ZR_POK_NAME + 1];
    zr_pok_poke *pokes;
    size_t count;
    int usable;                             /* 0: damaged, see `line` and `problem` */
    unsigned line;                          /* the line of the trainer's N, or of the first thing wrong with it */
    char problem[48];
} zr_pok_trainer;

typedef struct zr_pok {
    zr_pok_trainer *trainers;
    size_t count;
} zr_pok;

/* Reads the bytes of a file. 1 with the trainers in *pok (free with zr_pok_free), or 0 with the reason. */
int zr_pok_parse(const uint8_t *bytes, size_t size, zr_pok *pok, char *error, size_t capacity);
void zr_pok_free(zr_pok *pok);

#endif /* ZR_POK_H */
