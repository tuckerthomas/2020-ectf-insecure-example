 /*
  * eCTF Collegiate 2020 MicroBlaze Example Code
 * Audio Digital Rights Management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "xparameters.h"
#include "xil_exception.h"
#include "xstatus.h"
#include "xaxidma.h"
#include "xil_mem.h"
#include "util.h"
#include "secrets.h"
#include "xintc.h"
#include "constants.h"
#include "sleep.h"

#include <bearssl.h>

//////////////////////// GLOBALS ////////////////////////

// audio DMA access
static XAxiDma sAxiDma;

// LED colors and controller
u32 *led = (u32*) XPAR_RGB_PWM_0_PWM_AXI_BASEADDR;
const struct color RED =    {0x01ff, 0x0000, 0x0000};
const struct color YELLOW = {0x01ff, 0x01ff, 0x0000};
const struct color GREEN =  {0x0000, 0x01ff, 0x0000};
const struct color BLUE =   {0x0000, 0x0000, 0x01ff};

// change states
#define change_state(state, color) c->drm_state = state; s.drm_state = state; setLED(led, color);
#define set_stopped() change_state(STOPPED, RED)
#define set_working() change_state(WORKING, YELLOW)
#define set_playing() change_state(PLAYING, GREEN)
#define set_paused()  change_state(PAUSED, BLUE)
#define set_waiting_metadata() change_state(WAITING_METADATA, YELLOW)
#define set_waiting_chunk() change_state(WAITING_CHUNK, YELLOW)
#define set_reading_chunk() change_state(READING_CHUNK, YELLOW)

// shared command channel -- read/write for both PS and PL
volatile cmd_channel *c = (cmd_channel*)SHARED_DDR_BASE;

// internal state store
internal_state s;

//////////////////////// INTERRUPT HANDLING ////////////////////////

// shared variable between main thread and interrupt processing thread
volatile static int InterruptProcessed = FALSE;
static XIntc InterruptController;

void myISR(void) {
    InterruptProcessed = TRUE;
}


//////////////////////// UTILITY FUNCTIONS ////////////////////////

// returns whether an rid has been provisioned
int is_provisioned_rid(char rid) {
    for (int i = 0; i < NUM_PROVISIONED_REGIONS; i++) {
        if (rid == provisioned_rid[i].provisioned_regionID) {
            return TRUE;
        }
    }
    return FALSE;
}

// looks up the region name corresponding to the rid
int rid_to_region_name(u32 rid, char **region_name, int provisioned_only) {
    for (int i = 0; i < NUM_REGIONS; i++) {
        if (rid == device_regions[i].regionID &&
            (!provisioned_only || is_provisioned_rid(rid))) {
            *region_name = (char *)device_regions[i].regionName;
            return TRUE;
        }
    }

    mb_printf("Could not find region ID '%d'\r\n", rid);
    *region_name = "<unknown region>";
    return FALSE;
}


// looks up the rid corresponding to the region name
int region_name_to_rid(char *region_name, char *rid, int provisioned_only) {
    for (int i = 0; i < NUM_REGIONS; i++) {
        if (!strcmp(region_name, device_regions[i].regionName) &&
            (!provisioned_only || is_provisioned_rid(device_regions[i].regionID))) {
            *rid = device_regions[i].regionID;
            return TRUE;
        }
    }

    mb_printf("Could not find region name '%s'\r\n", region_name);
    *rid = -1;
    return FALSE;
}


// returns whether a uid has been provisioned
int is_provisioned_uid(char uid) {
    for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
        if (uid == provisioned_uid[i].provisioned_userID) {
            return TRUE;
        }
    }
    return FALSE;
}


// looks up the username corresponding to the uid
int uid_to_username(u32 uid, char **username, int provisioned_only) {
    for (int i = 0; i < NUM_USERS; i++) {
        if (uid == device_users[i].uid &&
            (!provisioned_only || is_provisioned_uid(uid))) {
            *username = (char *)device_users[i].username;
            return TRUE;
        }
    }

    mb_printf("Could not find uid '%d'\r\n", uid);
    *username = "<unknown user>";
    return FALSE;
}


// looks up the uid corresponding to the username
int username_to_uid(char *username, u8 *uid, int provisioned_only) {
    for (int i = 0; i < NUM_USERS; i++) {
        if (!strcmp(username, device_users[i].username) &&
            (!provisioned_only || is_provisioned_uid(device_users[i].uid))) {
            *uid = device_users[i].uid;
            return TRUE;
        }
    }

    mb_printf("Could not find username '%s'\r\n", username);
    *uid = -1;
    return FALSE;
}


// loads the song metadata in the shared buffer into the local struct
void load_song_md() {
    s.song_md.md_size = c->song.md.md_size;
    s.song_md.owner_id = c->song.md.owner_id;
    s.song_md.num_regions = c->song.md.num_regions;
    s.song_md.num_users = c->song.md.num_users;
    memcpy(s.song_md.rids, (void *)get_drm_rids(c->song), s.song_md.num_regions);
    memcpy(s.song_md.uids, (void *)get_drm_uids(c->song), s.song_md.num_users);
}

// checks if the song loaded into the shared buffer is locked for the current user
int is_locked() {
    int locked = TRUE;

    // check for authorized user
    if (!s.logged_in) {
        mb_printf("No user logged in");
    } else {
        load_song_md();

        // check if user is authorized to play song
        if (s.uid == s.song_md.owner_id) {
            locked = FALSE;
        } else {
            for (int i = 0; i < NUM_PROVISIONED_USERS && locked; i++) {
                if (s.uid == s.song_md.uids[i]) {
                    locked = FALSE;
                }
            }
        }

        if (locked) {
            mb_printf("User '%s' does not have access to this song", s.username);
            return locked;
        }
        mb_printf("User '%s' has access to this song", s.username);
        locked = TRUE; // reset lock for region check

        // search for region match
        for (int i = 0; i < s.song_md.num_regions; i++) {
            for (int j = 0; j < (u8)NUM_PROVISIONED_REGIONS; j++) {
                if (provisioned_rid[j].provisioned_regionID == s.song_md.rids[i]) {
                    locked = FALSE;
                }
            }
        }

        if (!locked) {
            mb_printf("Region Match. Full Song can be played. Unlocking...");
        } else {
            mb_printf("Invalid region");
        }
    }
    return locked;
}


// copy the local song metadata into buf in the correct format
// returns the size of the metadata in buf (including the metadata size field)
// song metadata should be loaded before call
int gen_song_md(char *buf) {
    buf[0] = ((5 + s.song_md.num_regions + s.song_md.num_users) / 2) * 2; // account for parity
    buf[1] = s.song_md.owner_id;
    buf[2] = s.song_md.num_regions;
    buf[3] = s.song_md.num_users;
    memcpy(buf + 4, s.song_md.rids, s.song_md.num_regions);
    memcpy(buf + 4 + s.song_md.num_regions, s.song_md.uids, s.song_md.num_users);

    return buf[0];
}

static size_t hextobin(unsigned char *dst, const char *src) {
	size_t num;
	unsigned acc;
	int z;

	num = 0;
	z = 0;
	acc = 0;
	while (*src != 0) {
		int c = *src++;
		if (c >= '0' && c <= '9') {
			c -= '0';
		} else if (c >= 'A' && c <= 'F') {
			c -= ('A' - 10);
		} else if (c >= 'a' && c <= 'f') {
			c -= ('a' - 10);
		} else {
			continue;
		}
		if (z) {
			*dst++ = (acc << 4) + c;
			num++;
		} else {
			acc = c;
		}
		z = !z;
	}
	return num;
}

void hash_pin(const char *pin, const char *salt, unsigned char *hashpinBuffer) {
	char concatPin[MAX_PIN_SZ + SALT_SZ];

	strncpy(concatPin, pin, MAX_PIN_SZ);
	strncat(concatPin, salt, SALT_SZ);

    br_sha256_context ctx;

    br_sha256_init(&ctx); // TODO: Check

    br_sha256_update(&ctx, concatPin, strlen(concatPin));

    br_sha256_out(&ctx, hashpinBuffer);
}

unsigned int read_header(unsigned char *key, waveHeaderMetaStruct *waveHeaderMeta) {
	unsigned char nonce[NONCE_SIZE], tag[MAC_SIZE];
	unsigned char aad[12] = "wave_header";
	unsigned char tag_buffer[MAC_SIZE];

	set_working();

	memcpy(nonce, (void *)&(c->encWaveHeaderMeta.nonce), NONCE_SIZE);
	memcpy(waveHeaderMeta, (void *)&(c->encWaveHeaderMeta.wave_header_meta), sizeof(waveHeaderMetaStruct));
	memcpy(tag, (void *)&(c->encWaveHeaderMeta.tag), MAC_SIZE);

	br_poly1305_ctmul_run(key, nonce, waveHeaderMeta, ENC_WAVE_HEADER_SZ, aad, sizeof(aad), tag_buffer, br_chacha20_ct_run, 0);

	if (memcmp(tag_buffer, tag, MAC_SIZE) == 0) {
		mb_printf("File header validated\r\n");
		// Continue Decryption
		set_waiting_metadata();
		return waveHeaderMeta->metadata_size;
	} else {
		mb_printf("Modification detected!\r\n");
		set_stopped();
		return -1;
	}

	return 1;
}

int read_metadata(unsigned char *key, encryptedMetadata *metadata) {
	unsigned char nonce[NONCE_SIZE], tag[MAC_SIZE];
	unsigned char aad[10] = "meta_data";
	unsigned char tag_buffer[MAC_SIZE];
	unsigned char metadata_buffer[METADATA_SZ];

	memcpy(nonce, (unsigned char *)&(c->encMetadata.nonce), NONCE_SIZE);
	memcpy(tag, (unsigned char *)&(c->encMetadata.tag), MAC_SIZE);
	memcpy(metadata_buffer, (unsigned char *) &(c->encMetadata.metadata), METADATA_SZ);
	//memcpy(metadata_buffer, get_metadata(c->encMetadata), metadata_size);

	br_poly1305_ctmul_run(key, nonce, metadata_buffer, METADATA_SZ, aad, sizeof(aad), tag_buffer, br_chacha20_ct_run, 0);

	if (memcmp(tag_buffer, tag, MAC_SIZE) == 0) {
		mb_printf("Metadata validated\r\n");
		// Copy metadata into local state
		memcpy(&s.purdue_md, metadata_buffer, METADATA_SZ);
		return 0;
	} else {
		mb_printf("Modification detected!\r\n");
		set_stopped();
		return -1;
	}
	return 1;
}

int read_chunks(unsigned char *key, int chunk_size, int chunk_num, int buffer_loc, u32 *fifo_ptr) {
	static unsigned char encrypt_chunk_buffer[SONG_CHUNK_SZ];
	//mb_printf("Reading chunk %i, with chunk_size: %i \r\n", chunk_num, chunk_size);
	unsigned char nonce[NONCE_SIZE], tag[MAC_SIZE];

	int aad = chunk_num;

	unsigned char tag_buffer[MAC_SIZE];

	memcpy(nonce, (unsigned char *) &c->encSongBuffer[buffer_loc].nonce, NONCE_SIZE);
	memcpy(encrypt_chunk_buffer, (unsigned char *) &c->encSongBuffer[buffer_loc].data, SONG_CHUNK_SZ);
	memcpy(tag, (unsigned char *) &c->encSongBuffer[buffer_loc].tag, MAC_SIZE);

	br_poly1305_ctmul_run(key, nonce, encrypt_chunk_buffer, chunk_size, &aad, sizeof(aad), tag_buffer, br_chacha20_ct_run, 0);

	if (memcmp(tag_buffer, tag, MAC_SIZE) == 0) {
		u32 counter = 0, rem, cp_num, cp_xfil_cnt, offset, dma_cnt, length;
	    //mb_printf("Reading Audio File...");
	    load_song_md();

	    length = chunk_size;

	    rem = length;
		while (rem > 0) {
			// calculate write size and offset
			cp_num = (rem > CHUNK_SZ) ? CHUNK_SZ : rem;
			offset = (counter++ % 2 == 0) ? 0 : CHUNK_SZ;

			// do first mem cpy here into DMA BRAM
			Xil_MemCpy(
					(void *) (XPAR_MB_DMA_AXI_BRAM_CTRL_0_S_AXI_BASEADDR + offset),
					(void *) (&encrypt_chunk_buffer + length - rem),
					(u32) (cp_num));

			cp_xfil_cnt = cp_num;

			while (cp_xfil_cnt > 0) {
				// polling while loop to wait for DMA to be ready
				// DMA must run first for this to yield the proper state
				// rem != length checks for first run
				while (XAxiDma_Busy(&sAxiDma, XAXIDMA_DMA_TO_DEVICE) && rem != length && *fifo_ptr < (FIFO_CAP - 32));

				// do DMA
				dma_cnt = (FIFO_CAP - *fifo_ptr > cp_xfil_cnt) ? FIFO_CAP - *fifo_ptr : cp_xfil_cnt;
				fnAudioPlay(sAxiDma, offset, dma_cnt);
				cp_xfil_cnt -= dma_cnt;
			}

			rem -= cp_num;
		}
		return 0;
	} else {
		mb_printf("The tags are not the same :( \r\n");
		mb_printf("Chunk %i failed", chunk_num);
		mb_printf("Modification detected!\r\n");
		set_stopped();
		return -1;
	}

	return 1;
}

int toggle_offset(int offset) {
	if (!offset) {
		offset = 1;
	} else {
		offset = 0;
	}

	return offset;
}

void encryptMetaData(unsigned char *key, char *metadata, encryptedMetadata *enc_metadata) {
	char nonce[NONCE_SIZE];
	char aad[] = "meta_data";
	char tag_buffer[MAC_SIZE];
    
	// Start nonce calculation
    br_sha256_context *ctx;

    br_sha256_init(ctx); // TODO: Check

    if (ctx == NULL) {
    	mb_printf("SHA256 Init failed\r\n");
    }

    br_sha256_update(ctx, metadata, METADATA_SZ);
    char sha_compute[br_sha256_SIZE];
    br_sha256_out(ctx, sha_compute);

    // Pull the first 16 bytes for the nonce
    memcpy(nonce, sha_compute, NONCE_SIZE);

    // Encrypt the metadata
	br_poly1305_ctmul_run(key, nonce, metadata, METADATA_SZ, &aad, sizeof(aad), tag_buffer, br_chacha20_ct_run, 1);

	// Copy encrypted metadata to the command buffer
	memcpy(enc_metadata->nonce, nonce, NONCE_SIZE);
	memcpy(enc_metadata->metadata, metadata, METADATA_SZ);
	memcpy(enc_metadata->tag, tag_buffer, MAC_SIZE);

	return;
}

//////////////////////// COMMAND FUNCTIONS ////////////////////////

// attempt to log in to the credentials in the shared buffer
void login() {
    if (s.logged_in) {
        mb_printf("Already logged in. Please log out first.\r\n");
        memcpy((void*)c->username, s.username, USERNAME_SZ);
        memcpy((void*)c->pin, s.pin, MAX_PIN_SZ);
    } else {
        for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
            // search for matching username
            if (!strcmp((void*)c->username, device_users[i].username)) {
                
                //MAKE FUNCTIONAL WITH HASHED VALUES
            	unsigned char hashedPin[32];
            	unsigned char binHash[32];

            	hextobin(binHash, device_users[i].hashedPin);

            	hash_pin((const char *)c->pin, device_users[i].salt, hashedPin);
            	if (!strncmp(hashedPin, binHash, 32)) {
                    //update states
                    s.logged_in = 1;
                    c->login_status = 1;
                    // TODO: Change
                    memcpy(s.username, (void*)c->username, USERNAME_SZ);
                    memcpy(s.pin, (void*)c->pin, MAX_PIN_SZ);
                    s.uid = provisioned_uid[i].provisioned_userID;
                    mb_printf("Logged in for user '%s'\r\n", c->username);
                    return;
                } else {
                    // reject login attempt
                    mb_printf("Incorrect pin for user '%s'\r\n", c->username);
                    memset((void*)c->username, 0, USERNAME_SZ);
                    memset((void*)c->pin, 0, MAX_PIN_SZ);
                    return;
                }
            }
        }

        // reject login attempt
        mb_printf("User not found\r\n");
        memset((void*)c->username, 0, USERNAME_SZ);
        memset((void*)c->pin, 0, MAX_PIN_SZ);
    }
}


// attempt to log out
void logout() {
    if (c->login_status) {
        mb_printf("Logging out...\r\n");
        s.logged_in = 0;
        c->login_status = 0;
        memset((void*)c->username, 0, USERNAME_SZ);
        memset((void*)c->pin, 0, MAX_PIN_SZ);
        s.uid = 0;
    } else {
        mb_printf("Not logged in\r\n");
    }
}


// handles a request to query the player's metadata
void query_player() {
    c->query.num_regions = NUM_PROVISIONED_REGIONS;
    c->query.num_users = NUM_PROVISIONED_USERS;

    for (int i = 0; i < NUM_PROVISIONED_REGIONS; i++) {
        strcpy((char *)q_region_lookup(c->query, i), device_regions[i].regionName);
    }

    for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
        strcpy((char *)q_user_lookup(c->query, i), device_users[i].username);
    }

    mb_printf("Queried player (%d regions, %d users)\r\n", c->query.num_regions, c->query.num_users);
}


// handles a request to query song metadata
void query_song() {
    char *name;

    // load song
    load_song_md();
    memset((void *)&c->query, 0, sizeof(query));

    c->query.num_regions = s.song_md.num_regions;
    c->query.num_users = s.song_md.num_users;

    // copy owner name
    uid_to_username(s.song_md.owner_id, &name, FALSE);
    strcpy((char *)c->query.owner, name);

    // copy region names
    for (int i = 0; i < s.song_md.num_regions; i++) {
        rid_to_region_name(s.song_md.rids[i], &name, FALSE);
        strcpy((char *)q_region_lookup(c->query, i), name);
    }

    // copy authorized uid names
    for (int i = 0; i < s.song_md.num_users; i++) {
        uid_to_username(s.song_md.uids[i], &name, FALSE);
        strcpy((char *)q_user_lookup(c->query, i), name);
    }

    mb_printf("Queried song (%d regions, %d users)\r\n", c->query.num_regions, c->query.num_users);
}

// handles a request to query song metadata
void query_enc_song(unsigned char *key) {
    char *name;

    // Decrypt metadata and set to internal state
    encryptedMetadata metadata;
    if (read_metadata(key, &metadata) != 0) {
    	mb_printf("Could not read metadata!\r\n");
    	return;
    }

    // Copy data into new metadata
    memset((void *)&c->query, 0, sizeof(query));

    c->query.num_regions = s.purdue_md.num_regions;
    c->query.num_users = s.purdue_md.num_users;

    // copy owner name
    uid_to_username(s.purdue_md.owner_id, &name, FALSE);
    strcpy((char *)c->query.owner, name);

    // copy region names
    for (int i = 0; i < s.purdue_md.num_regions; i++) {
        rid_to_region_name(s.purdue_md.provisioned_regions[i], &name, FALSE);
        strcpy((char *)q_region_lookup(c->query, i), name);
    }

    // copy authorized uid names
    for (int i = 0; i < s.purdue_md.num_users; i++) {
        uid_to_username(s.purdue_md.provisioned_users[i], &name, FALSE);
        strcpy((char *)q_user_lookup(c->query, i), name);
    }

    mb_printf("Queried song (%d regions, %d users)\r\n", c->query.num_regions, c->query.num_users);
}


// add a user to the song's list of users
void share_song() {
    int new_md_len, shift;
    char new_md[256], uid;

    // reject non-owner attempts to share
    load_song_md();
    if (!s.logged_in) {
        mb_printf("No user is logged in. Cannot share song\r\n");
        c->song.wav_size = 0;
        return;
    } else if (s.uid != s.song_md.owner_id) {
        mb_printf("User '%s' is not song's owner. Cannot share song\r\n", s.username);
        c->song.wav_size = 0;
        return;
    } else if (!username_to_uid((char *)c->username, &uid, TRUE)) {
        mb_printf("Username not found\r\n");
        c->song.wav_size = 0;
        return;
    }

    // generate new song metadata
    s.song_md.uids[s.song_md.num_users++] = uid;
    new_md_len = gen_song_md(new_md);
    shift = new_md_len - s.song_md.md_size;

    // shift over song and add new metadata
    if (shift) {
        memmove((void *)get_drm_song(c->song) + shift, (void *)get_drm_song(c->song), c->song.wav_size);
    }
    memcpy((void *)&c->song.md, new_md, new_md_len);

    // update file size
    c->song.file_size += shift;
    c->song.wav_size  += shift;

    mb_printf("Shared song with '%s'\r\n", c->username);
}

// add a user to the song's list of users
void share_enc_song(unsigned char *key) {
    u8 uid;

    encryptedMetadata metadata;
    if (read_metadata(key, &metadata) != 0) {
    	mb_printf("Metadta could not be validated \r\n");
    	return;
    }

    // Check if a user is logged in
    if (!s.logged_in) {
        mb_printf("No user is logged in. Cannot share song\r\n");
        c->song.wav_size = 0;
        return;
    // Check if the user that is logged in is the owner of the song
    } else if (s.uid != s.purdue_md.owner_id) {
        mb_printf("User '%s' is not song's owner. Cannot share song\r\n", s.username);
        c->song.wav_size = 0;
        return;
    // Check if the username is a valid user
    } else if (!username_to_uid((char *)c->username, &uid, TRUE)) {
        mb_printf("Username not found\r\n");
        c->song.wav_size = 0;
        return;
    }

    // Create spot for new metadata
    purdue_md newMetaData;

    // Copy data into new metadata
    newMetaData.owner_id = s.purdue_md.owner_id;
    newMetaData.num_regions = s.purdue_md.num_regions;
    newMetaData.num_users = s.purdue_md.num_users;

    // TODO: Check to set if there's already a max amount of users
    // Increase shared users
    for (int i = 0; i < s.purdue_md.num_users; i++) {
    	newMetaData.provisioned_users[i] = s.purdue_md.provisioned_users[i];
    }

    // Add the new userid
    newMetaData.provisioned_regions[newMetaData.num_users++] = uid;

    // Prepare the new metadata to be encrypted
    char metadata_buffer[METADATA_SZ];

    // Encrypt the new metadata and copy it into the command buffer
    encryptMetaData(key, metadata_buffer, (encryptedMetadata *)&c->encMetadata);

    mb_printf("Shared song with '%s'\r\n", c->username);
}


// plays a song and looks for play-time commands
void play_song() {
    u32 counter = 0, rem, cp_num, cp_xfil_cnt, offset, dma_cnt, length, *fifo_fill;

    mb_printf("Reading Audio File...");
    load_song_md();

    // get WAV length
    length = c->song.wav_size;
    mb_printf("Song length = %dB", length);

    // truncate song if locked
    if (length > PREVIEW_SZ && is_locked()) {
        length = PREVIEW_SZ;
        mb_printf("Song is locked.  Playing only %ds = %dB\r\n",
                   PREVIEW_TIME_SEC, PREVIEW_SZ);
    } else {
        mb_printf("Song is unlocked. Playing full song\r\n");
    }

    rem = length;
    fifo_fill = (u32 *)XPAR_FIFO_COUNT_AXI_GPIO_0_BASEADDR;

    // write entire file to two-block codec fifo
    // writes to one block while the other is being played
    set_playing();
    while(rem > 0) {
        // check for interrupt to stop playback
        while (InterruptProcessed) {
            InterruptProcessed = FALSE;

            switch (c->cmd) {
            case PAUSE:
                mb_printf("Pausing... \r\n");
                set_paused();
                while (!InterruptProcessed) continue; // wait for interrupt
                break;
            case PLAY:
                mb_printf("Resuming... \r\n");
                set_playing();
                break;
            case STOP:
                mb_printf("Stopping playback...");
                return;
            case RESTART:
                mb_printf("Restarting song... \r\n");
                rem = length; // reset song counter
                set_playing();
            default:
                break;
            }
        }

        // calculate write size and offset
        cp_num = (rem > CHUNK_SZ) ? CHUNK_SZ : rem;
        offset = (counter++ % 2 == 0) ? 0 : CHUNK_SZ;

        // do first mem cpy here into DMA BRAM
        Xil_MemCpy((void *)(XPAR_MB_DMA_AXI_BRAM_CTRL_0_S_AXI_BASEADDR + offset),
                   (void *)(get_drm_song(c->song) + length - rem),
                   (u32)(cp_num));

        cp_xfil_cnt = cp_num;

        while (cp_xfil_cnt > 0) {

            // polling while loop to wait for DMA to be ready
            // DMA must run first for this to yield the proper state
            // rem != length checks for first run
            while (XAxiDma_Busy(&sAxiDma, XAXIDMA_DMA_TO_DEVICE)
                   && rem != length && *fifo_fill < (FIFO_CAP - 32));

            // do DMA
            dma_cnt = (FIFO_CAP - *fifo_fill > cp_xfil_cnt)
                      ? FIFO_CAP - *fifo_fill
                      : cp_xfil_cnt;
            fnAudioPlay(sAxiDma, offset, dma_cnt);
            cp_xfil_cnt -= dma_cnt;
        }

        rem -= cp_num;
    }
}


// removes DRM data from song for digital out
void digital_out() {
    // remove metadata size from file and chunk sizes
    c->song.file_size -= c->song.md.md_size;
    c->song.wav_size -= c->song.md.md_size;

    if (is_locked() && PREVIEW_SZ < c->song.wav_size) {
        mb_printf("Only playing 30 seconds");
        c->song.file_size -= c->song.wav_size - PREVIEW_SZ;
        c->song.wav_size = PREVIEW_SZ;
    }

    // move WAV file up in buffer, skipping metadata
    mb_printf(MB_PROMPT "Dumping song (%dB)...", c->song.wav_size);
    memmove((void *)&c->song.md, (void *)get_drm_song(c->song), c->song.wav_size);

    mb_printf("Song dump finished\r\n");
}

void play_encrypted_song(unsigned char *key) {
	waveHeaderMetaStruct waveHeaderMeta;

	mb_printf("Chunk size set to: %i", SONG_CHUNK_SZ);

	int metadata_size = read_header(key, &waveHeaderMeta);
	if (metadata_size == -1) {
		mb_printf("Song not valid!\r\n");
		return;
	}
	c->metadata_size = metadata_size;

	mb_printf("Waiting for metadata!\r\n");

	set_waiting_metadata();

	int chunks_to_read, chunk_counter = 1;
	unsigned int chunk_remainder;

	chunks_to_read = waveHeaderMeta.wave_header.wav_size / SONG_CHUNK_SZ;
	chunk_remainder = waveHeaderMeta.wave_header.wav_size % SONG_CHUNK_SZ;

	encryptedMetadata metadata;

	// Initialize buffer offset;
	int buffer_offset = 0;

	u32 *fifo_fill = (u32 *)XPAR_FIFO_COUNT_AXI_GPIO_0_BASEADDR;

	while (1) {
		//mb_printf("In Play loop\r\n");
		while (InterruptProcessed) {
			InterruptProcessed = FALSE;

			mb_printf("Processing interruption\r\n");

			set_working();

			switch (c->cmd) {
			case READ_METADATA:
				if (read_metadata(key, &metadata) == 0) {
					c->total_chunks = chunks_to_read;
					c->chunk_size = SONG_CHUNK_SZ;
					c->chunk_remainder = chunk_remainder;
					set_waiting_chunk();
					break;
				} else {
					return;
				}
			case READ_CHUNK:
				for (int i = 0; i < ENC_BUFFER_SZ / 2; i++) {
					int buffer_loc = i + ((ENC_BUFFER_SZ / 2) * buffer_offset);
					if (chunk_counter < chunks_to_read) {
						if (read_chunks(key, SONG_CHUNK_SZ, chunk_counter, buffer_loc, fifo_fill) == 0) {
							chunk_counter++;
						} else {
							return;
						}
					} else if (chunk_counter == chunks_to_read) {
						if (read_chunks(key, chunk_remainder, chunk_counter, buffer_loc, fifo_fill) == 0) {
							break;
						} else {
							return;
						}
					} else {
						return;
					}
				}
				mb_printf("Finished reading %d chunks\r\n", ENC_BUFFER_SZ / 2);
				// Toggle offset
				c->buffer_offset = buffer_offset;
				buffer_offset = toggle_offset(buffer_offset);
				mb_printf("Requesting more chunks\r\n");
				set_waiting_chunk();
				break;
			case STOP:
				return;
			default:
				break;
			}
		}
	}
	// TODO: Check if song chunks can be played without file headers
	// TODO: Make sure playing a song follows original checks, IE: user logged in/song is shared with them/they own the song/can be played in that region
}


//////////////////////// MAIN ////////////////////////


int main() {
    u32 status;

    init_platform();
    microblaze_register_handler((XInterruptHandler)myISR, (void *)0);
    microblaze_enable_interrupts();

    // Initialize the interrupt controller driver so that it is ready to use.
    status = XIntc_Initialize(&InterruptController, XPAR_INTC_0_DEVICE_ID);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    // Set up the Interrupt System.
    status = SetUpInterruptSystem(&InterruptController, (XInterruptHandler)myISR);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    // Congigure the DMA
    status = fnConfigDma(&sAxiDma);
    if(status != XST_SUCCESS) {
        mb_printf("DMA configuration ERROR\r\n");
        return XST_FAILURE;
    }

    // Start the LED
    enableLED(led);
    set_stopped();

    // clear command channel
    memset((void*)c, 0, sizeof(cmd_channel));

    mb_printf("Audio DRM Module has Booted\n\r");
    // Load keys/secrets
    unsigned char key[32];

    hextobin(key, KEY_HEX);

    // Handle commands forever
    while(1) {
        // wait for interrupt to start
        if (InterruptProcessed) {
            InterruptProcessed = FALSE;

            set_working();

            // c->cmd is set by the miPod player
            switch (c->cmd) {
            case LOGIN:
                login();
                break;
            case LOGOUT:
                logout();
                break;
            case QUERY_PLAYER:
                query_player();
                break;
            case QUERY_SONG:
                query_song();
                break;
            case QUERY_ENC_SONG:
            	query_enc_song(key);
            	break;
            case SHARE:
                share_song();
                break;
            case ENC_SHARE:
            	share_enc_song(key);
            	break;
            case PLAY:
                play_song();
                mb_printf("Done Playing Song\r\n");
                break;
            case DIGITAL_OUT:
                digital_out();
                break;
            case READ_HEADER:
            	play_encrypted_song(key);
            	break;
            default:
                break;
            }

            // reset statuses and sleep to allow player to recognize WORKING state
            strcpy((char *)c->username, s.username);
            c->login_status = s.logged_in;
            usleep(500);
            set_stopped();
        }
    }

    cleanup_platform();
    return 0;
}
