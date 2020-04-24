/*
 * miPod.h
 *
 *  Created on: Jan 9, 2020
 *      Author: ectf
 */

#ifndef SRC_MIPOD_H_
#define SRC_MIPOD_H_

#import <stdint.h>

// miPod constants
#define USR_CMD_SZ 64

// protocol constants
#define MAX_REGIONS 32
#define REGION_NAME_SZ 16
#define MAX_USERS 64
#define USERNAME_SZ 16
#define MAX_PIN_SZ 8
#define MAX_SONG_SZ (1<<25)

#define HASHPIN_SZ 32
#define SALT_SZ 7

// printing utility
#define MP_PROMPT "MP> "
#define mp_printf(...) printf(MP_PROMPT __VA_ARGS__)

#define USER_PROMPT "miPod %s# "
#define print_prompt() printf(USER_PROMPT, "")
#define print_prompt_msg(...) printf(USER_PROMPT, __VA_ARGS__)

#define AUDIO_SAMPLING_RATE 48000
#define BYTES_PER_SAMP 2
#define NONCE_SIZE 12
#define MAC_SIZE 16
#define WAVE_HEADER_SZ 44
#define METADATA_SZ 390
#define ENC_WAVE_HEADER_SZ WAVE_HEADER_SZ + NONCE_SIZE + MAC_SIZE
#define ENC_METADATA_SZ METADATA_SZ + NONCE_SIZE + MAC_SIZE
#define META_DATA_ALLOC 4
#define SONG_CHUNK_SZ 16000
#define ENC_BUFFER_SZ 60
#define SHA_256_SUM_SZ 32

// structs to import secrets.h JSON data into memory
typedef struct {
    uint32_t uid;
    char username[USERNAME_SZ];
    char hashedPin[HASHPIN_SZ];
    char salt[SALT_SZ];
} user_struct;

typedef struct {
    uint32_t regionID;
    char regionName[REGION_NAME_SZ];
} region_struct;

typedef struct {
    uint32_t provisioned_userID;
} provisioned_user_struct;

typedef struct {
    uint32_t provisioned_regionID;
} provisioned_region_struct;

// struct to interpret shared buffer as a query
typedef struct {
	uint32_t num_regions;
	uint32_t num_users;
    char owner[USERNAME_SZ];
    char regions[MAX_REGIONS * REGION_NAME_SZ];
    char users[MAX_USERS * USERNAME_SZ];
} queryStruct;

// simulate array of 64B names without pointer indirection
#define q_region_lookup(q, i) (q.regions + (i * REGION_NAME_SZ))
#define q_user_lookup(q, i) (q.users + (i * USERNAME_SZ))

typedef struct __attribute__ ((__packed__)) {
	unsigned char sha256sum[SHA_256_SUM_SZ];
    uint32_t owner_id;
    uint8_t num_regions;
    uint8_t num_users;
    uint32_t provisioned_regions[MAX_REGIONS];
    uint32_t provisioned_users[MAX_USERS];
} purdue_md;

typedef struct __attribute__ ((__packed__)) {
	unsigned char wav_header[WAVE_HEADER_SZ];
	uint32_t metadata_size;
} waveHeaderStruct;

typedef struct __attribute__ ((__packed__)) {
	unsigned char nonce[NONCE_SIZE];
	waveHeaderStruct wave_header_struct;
	unsigned char tag[MAC_SIZE];
} encryptedWaveheader;

typedef struct __attribute__ ((__packed__)) {
	unsigned char nonce[NONCE_SIZE];
	unsigned char tag[MAC_SIZE];
	unsigned char metadata[METADATA_SZ];
} encryptedMetadata;

#define get_metadata(m) ((unsigned char *)&m + NONCE_SIZE + MAC_SIZE)

typedef struct __attribute__ ((__packed__)) {
	unsigned char nonce[NONCE_SIZE];
	unsigned char tag[MAC_SIZE];
	unsigned char data[SONG_CHUNK_SZ];
} encryptedSongChunk;

#define get_chunk_data(c) ((char *)(&c.data))

// accessors for variable-length metadata fields
#define get_drm_rids(d) (d.md.buf)
#define get_drm_uids(d) (d.md.buf + d.md.num_regions)
#define get_drm_song(d) ((char *)(&d.md) + d.md.md_size)

// TODO: Remove deprecated commands
// shared buffer values
enum commands { QUERY_PLAYER, QUERY_SONG, LOGIN, LOGOUT, SHARE, PLAY, STOP, DIGITAL_OUT, PAUSE, RESTART, FF, RW, PLAY_SONG, READ_HEADER, READ_METADATA, WAIT_FOR_CHUNK, READ_CHUNK, ENC_SHARE, QUERY_ENC_SONG };
enum states   { STOPPED, WORKING, PLAYING, PAUSED, WAITING_FILE_HEADER, WAITING_METADATA, WAITING_CHUNK, READING_CHUNK };


// struct to interpret shared command channel
typedef volatile struct __attribute__((__packed__)) {
    char cmd;                   // from commands enum
    char drm_state;             // from states enum
    char login_status;          // 0 = logged off, 1 = logged on
    char username[USERNAME_SZ]; // stores logged in or attempted username
    char pin[MAX_PIN_SZ];       // stores logged in or attempted pin
    uint8_t share_rejected;		// tells mipod if the share was rejected or not
    uint32_t metadata_size;		// stores size of the metadata
    uint32_t total_chunks;		// stores the total chunks to be decrypted
    uint32_t chunk_size;		// stores dynamic chunk size
    uint32_t chunk_nums;
    uint32_t chunk_remainder;
    uint32_t buffer_offset;		// Determines if reading/writing to buffer
    unsigned char wav_header[WAVE_HEADER_SZ];
    unsigned char songBuffer[ENC_BUFFER_SZ * SONG_CHUNK_SZ];

    // shared buffer is either a drm song or a query
    union {
    	// Non-encrypted
        queryStruct query;

        // Encrypted
        encryptedWaveheader encWaveHeader;
        encryptedMetadata encMetadata;
        encryptedSongChunk encSongChunk;
        encryptedSongChunk encSongBuffer[ENC_BUFFER_SZ];
        char buf[MAX_SONG_SZ]; // sets correct size of cmd_channel for allocation
    };
} cmd_channel;

#endif /* SRC_MIPOD_H_ */
