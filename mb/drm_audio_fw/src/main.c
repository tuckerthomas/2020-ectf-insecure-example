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

// Bearssl Library
#include <bearssl_hash.h>

// Chacha20+poly1305 implementation
#include "chachapoly/chachapoly.h"

//////////////////////// GLOBALS ////////////////////////

// audio DMA access
static XAxiDma sAxiDma;

// LED colors and controller
u32 *led = (u32*) XPAR_RGB_PWM_0_PWM_AXI_BASEADDR;
const struct color RED =    {0x01ff, 0x0000, 0x0000};
const struct color YELLOW = {0x01ff, 0x01ff, 0x0000};
const struct color GREEN =  {0x0000, 0x01ff, 0x0000};
const struct color BLUE =   {0x0000, 0x0000, 0x01ff};

// DRM States and Colors associated with them
#define change_state(state, color) c->drm_state = state; s.drm_state = state; setLED(led, color);
#define set_stopped() change_state(STOPPED, RED)
#define set_working() change_state(WORKING, YELLOW)
#define set_playing() change_state(PLAYING, GREEN)
#define set_paused()  change_state(PAUSED, BLUE)
#define set_waiting_file_header() change_state(WAITING_FILE_HEADER, YELLOW)
#define set_waiting_metadata() change_state(WAITING_METADATA, YELLOW)
#define set_waiting_chunk() change_state(WAITING_CHUNK, YELLOW)
#define set_reading_chunk() change_state(READING_CHUNK, GREEN)

// shared command channel between microblaze and linux
volatile cmd_channel *c = (cmd_channel*)SHARED_DDR_BASE;

// internal state store
static internal_state s;

// Large chunk buffer
static unsigned char chunk_buffer[SONG_CHUNK_SZ];

//////////////////////// INTERRUPT HANDLING ////////////////////////

// shared variable between main thread and interrupt processing thread
volatile static int InterruptProcessed = FALSE;
static XIntc InterruptController;

void myISR(void) {
    InterruptProcessed = TRUE;
}


//////////////////////// UTILITY FUNCTIONS ////////////////////////

// returns whether an rid has been provisioned
int is_provisioned_rid(u32 rid) {
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
int is_provisioned_uid(u32 uid) {
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
int username_to_uid(char *username, u32 *uid, int provisioned_only) {
    for (int i = 0; i < NUM_USERS; i++) {
        if (!strncmp(username, device_users[i].username, strlen(device_users[i].username)) &&
            (!provisioned_only || is_provisioned_uid(device_users[i].uid))) {
            *uid = device_users[i].uid;
            return TRUE;
        }
    }

    mb_printf("Could not find username '%s'\r\n", username);
    *uid = -1;
    return FALSE;
}

// Converts hex string to binary array
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

// Hashes a given pin and stores it into the buffer
void hash_pin(const char *pin, const char *salt, unsigned char *hashpinBuffer) {
	char concatPin[MAX_PIN_SZ + SALT_SZ];
	memset(concatPin, 0, sizeof(concatPin));

	strncpy(concatPin, pin, strlen(pin));
	strncat(concatPin, salt, strlen(salt));

    br_sha256_context ctx;

    br_sha256_init(&ctx);

    br_sha256_update(&ctx, concatPin, strlen(concatPin));

    br_sha256_out(&ctx, hashpinBuffer);
}

// Validates a given encrypted waveHeader
unsigned int read_header(struct chachapoly_ctx *ctx, waveHeaderMetaStruct *waveHeaderMeta) {
	unsigned char nonce[NONCE_SIZE];
	unsigned char tag[MAC_SIZE];
	unsigned char aad[12] = "wave_header";

	//set_working();

	memcpy(nonce, (void *)&(c->encWaveHeaderMeta.nonce), NONCE_SIZE);
	memcpy(tag, (void *)&(c->encWaveHeaderMeta.tag), MAC_SIZE);

	int ret = chachapoly_crypt(ctx, nonce, &aad, sizeof(aad), (waveHeaderMetaStruct *) &c->encWaveHeaderMeta.wave_header_meta, sizeof(waveHeaderMetaStruct), waveHeaderMeta, tag, MAC_SIZE, 0);

	if (ret == CHACHAPOLY_OK) {
		mb_printf("File header validated\r\n");

		s.total_bytes_to_play = waveHeaderMeta->wave_header.wav_size;

		return waveHeaderMeta->metadata_size;
	} else {
		mb_printf("Header modification detected!\r\n");
		set_stopped();
		return -1;
	}

	return 1;
}

// Validates a given metadata
int read_metadata(struct chachapoly_ctx *ctx, encryptedMetadata *metadata) {
	unsigned char nonce[NONCE_SIZE];
	unsigned char tag[MAC_SIZE];
	unsigned char aad[10] = "meta_data";
	unsigned char metadata_buffer[METADATA_SZ];

	memcpy(nonce, (unsigned char *)&(c->encMetadata.nonce), NONCE_SIZE);
	memcpy(tag, (unsigned char *)&(c->encMetadata.tag), MAC_SIZE);

	int ret = chachapoly_crypt(ctx, nonce, &aad, sizeof(aad), (unsigned char *) &(c->encMetadata.metadata), METADATA_SZ, metadata_buffer, tag, MAC_SIZE, 0);

	if (ret == CHACHAPOLY_OK) {
		mb_printf("Metadata validated\r\n");
		// Copy metadata into local state
		memcpy(&s.purdue_md, metadata_buffer, METADATA_SZ);
		return 0;
	} else {
		mb_printf("Metadata modification detected!\r\n");
		set_stopped();
		return -1;
	}
	return 1;
}

// Read a chunk of specific size and number, decrypt and copy into the FIFO buffer
int read_chunks(struct chachapoly_ctx *ctx, unsigned char *chunk_buffer, unsigned char *sha256sum, int chunk_size, int chunk_num, int buffer_loc) {
	unsigned char nonce[NONCE_SIZE];
	unsigned char tag[MAC_SIZE];
	int aad = chunk_num;

	// Copy data to buffers
	memcpy(nonce, (unsigned char *) &c->encSongBuffer[buffer_loc].nonce, NONCE_SIZE);
	memcpy(tag, (unsigned char *) &c->encSongBuffer[buffer_loc].tag, MAC_SIZE);

	// Decrypt the chunk
	int ret = chachapoly_crypt(ctx, nonce, sha256sum, SHA_256_SUM_SZ, (unsigned char *)&c->encSongBuffer[buffer_loc].data, chunk_size, chunk_buffer, tag, MAC_SIZE, 0);

	if (ret == CHACHAPOLY_OK) {
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

// Toggle the offset for the chunk buffer
int toggle_offset(int offset) {
	if (!offset) {
		offset = 1;
	} else {
		offset = 0;
	}

	return offset;
}


// Calculate metadata hash, encrypt metadta and store into metadata buffer
void encryptMetaData(struct chachapoly_ctx *cha_ctx, char *metadata, encryptedMetadata *enc_metadata) {
	char nonce[NONCE_SIZE];
	char aad[] = "meta_data";
	char tag_buffer[MAC_SIZE];
    
	// Start nonce calculation
    br_sha256_context ctx;

    br_sha256_init(&ctx);

    // Calculate sha256 hash
    br_sha256_update(&ctx, metadata, METADATA_SZ);
    char sha_compute[br_sha256_SIZE];
    br_sha256_out(&ctx, sha_compute);

    // Pull the first 12 bytes for the nonce
    memcpy(nonce, sha_compute, NONCE_SIZE);

    // Encrypt the metadata
	chachapoly_crypt(cha_ctx, nonce, &aad, sizeof(aad), metadata, METADATA_SZ, enc_metadata->metadata, tag_buffer, MAC_SIZE, 1);

	// Copy encrypted metadata to the command buffer
	memcpy(enc_metadata->nonce, nonce, NONCE_SIZE);
	memcpy(enc_metadata->tag, tag_buffer, MAC_SIZE);

	return;
}

//////////////////////// COMMAND FUNCTIONS ////////////////////////

// attempt to log into the credentials in the shared buffer
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
                    // update states
                    s.logged_in = 1;
                    c->login_status = 1;

                    // Copy username, pin and uid to local state
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

    return;
}

// handles a request to query song metadata
void query_enc_song(unsigned char *key) {
    char *name;

    struct chachapoly_ctx ctx;
    chachapoly_init(&ctx, key, 256);

    // Decrypt metadata and set to internal state
    encryptedMetadata metadata;
    if (read_metadata(&ctx, &metadata) != 0) {
    	mb_printf("Could not read metadata!\r\n");
    	return;
    }

    // Copy data into new metadata
    memset((void *)&c->query, 0, sizeof(query));

    //purdue_md is the song metadata
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
void share_enc_song(unsigned char *key) {
    u32 uid;

    struct chachapoly_ctx ctx;
    chachapoly_init(&ctx, key, 256);

    encryptedMetadata metadata;
    if (read_metadata(&ctx, &metadata) != 0) {
    	mb_printf("Metadta could not be validated \r\n");
    	set_stopped();
    	return;
    }

    // Check if a user is logged in
    if (!s.logged_in) {
        mb_printf("No user is logged in. Cannot share song\r\n");
        c->share_rejected = 1;
        set_stopped();
		return;
    // Check if the user that is logged in is the owner of the song
    } else if (s.uid != s.purdue_md.owner_id) {
        mb_printf("User '%s' is not song's owner. Cannot share song\r\n", s.username);
        c->share_rejected = 1;
        set_stopped();
        return;
    // Check if the username is a valid user
    } else if (!username_to_uid((char *)c->username, &uid, TRUE)) {
        mb_printf("Username not found\r\n");
        c->share_rejected = 1;
        set_stopped();
        return;
    // Check if they own the song
    } else if(uid == s.purdue_md.owner_id){
        mb_printf("User is owner\r\n");
        c->share_rejected = 1;
        set_stopped();
		return;
	// Check if the song has already been shared to the max amount of users
	} else if(s.purdue_md.num_users == MAX_USERS) {
		mb_printf("User has already shared this song to the max amount of users\r\n");
		c->share_rejected = 1;
		set_stopped();
		return;
	}


	for(int i = 0; i < s.purdue_md.num_users; i++){
		if(uid == s.purdue_md.provisioned_users[i]){
       		mb_printf("User is already shared\r\n");
       		c->share_rejected = 1;
       		set_stopped();
			return;
		}
	}

    // Initialize empty struct
    // Create spot for new metadata
    static const purdue_md emptyMd;
    purdue_md newMetaData = emptyMd;

    // Copy data into new metadata
    memcpy(newMetaData.sha256sum, s.purdue_md.sha256sum, SHA_256_SUM_SZ);
    newMetaData.owner_id = s.purdue_md.owner_id;
    newMetaData.num_regions = s.purdue_md.num_regions;
    newMetaData.num_users = s.purdue_md.num_users;

    for (int i = 0; i < s.purdue_md.num_regions; i++) {
    	newMetaData.provisioned_regions[i] = s.purdue_md.provisioned_regions[i];
    }

    // Increase shared users
    for (int i = 0; i < s.purdue_md.num_users; i++) {
    	newMetaData.provisioned_users[i] = s.purdue_md.provisioned_users[i];
    }

    // Add the new userid
    newMetaData.provisioned_users[newMetaData.num_users++] = uid;

    // Prepare the new metadata to be encrypted
    char metadata_buffer[METADATA_SZ];

    memcpy(metadata_buffer, &newMetaData, sizeof(purdue_md));

    // Encrypt the new metadata and copy it into the command buffer
    encryptMetaData(&ctx, metadata_buffer, (encryptedMetadata *)&c->encMetadata);

    mb_printf("Shared song with '%s'\r\n", c->username);

    return;
}

// removes DRM data from song for digital out
void digital_out(unsigned char *key) {
	struct chachapoly_ctx ctx;
	chachapoly_init(&ctx, key, 256);

	waveHeaderMetaStruct waveHeaderMeta;
	encryptedMetadata metadata;

	// Metadata information
	int metadata_size = 0;
	int chunks_to_read, chunk_counter = 1;
	int chunk_remainder;

	// TODO: change buffer_offset to boolean
	// Initialize buffer offset;
	int buffer_offset = 0;						// Boolean for shared buffer offset
	int buffer_counter = 0;						// Counter for shared buffer location
	int chunks_decrypted = 0;					// Number of chunks decrypted

	set_waiting_file_header();

	while (1) {
		while (InterruptProcessed) {
			InterruptProcessed = FALSE;
			set_working();

			switch (c->cmd) {
			case READ_HEADER:
				metadata_size = read_header(&ctx, &waveHeaderMeta);
				if (metadata_size == -1) {
					mb_printf("Song not valid!\r\n");
					return;
				}

				c->metadata_size = metadata_size;

				// copy wave header to buffer
				memcpy((unsigned char *)&c->wave_header, &waveHeaderMeta.wave_header, WAVE_HEADER_SZ);

				set_waiting_metadata();

				chunks_to_read = waveHeaderMeta.wave_header.wav_size / SONG_CHUNK_SZ;
				chunk_remainder = waveHeaderMeta.wave_header.wav_size % SONG_CHUNK_SZ;
				break;
			case READ_METADATA:
				if (read_metadata(&ctx, &metadata) == 0) {
					c->total_chunks = chunks_to_read;
					c->chunk_size = SONG_CHUNK_SZ;
					c->chunk_remainder = chunk_remainder;
					set_waiting_chunk();
					break;
				} else {
					set_stopped();
					return;
				}
			default:
				break;
			}
		}

		// Still in play while loop
		if (c->cmd == READ_CHUNK) {
			if (chunks_decrypted == 0) {
				s.play_state = DECRYPT;
			}

			if (s.play_state == DECRYPT) {
				set_reading_chunk();

				int buffer_loc = buffer_counter++ + ((ENC_BUFFER_SZ / 2) * buffer_offset);

				// Check if on the last chunk
				int chunk_size = SONG_CHUNK_SZ;
				if (chunk_counter == chunks_to_read) {
					chunk_size = chunk_remainder;
				}

				if (read_chunks(&ctx, chunk_buffer, s.purdue_md.sha256sum, chunk_size, chunk_counter, buffer_loc) == 0) {
					memcpy((unsigned char *)&c->songBuffer[SONG_CHUNK_SZ * buffer_loc], chunk_buffer, chunk_size);
					chunk_counter++;
					chunks_decrypted++;

					if (chunk_counter + 1 == chunks_to_read) {
						set_stopped();
						return;
					}
				}
			}

			// Request more chunks
			if (s.play_state == REQUEST) {
				set_waiting_chunk();

				// Start decrypting more chunks
				s.play_state = DECRYPT;
			}

			// Check if reached the end of the command buffer
			// Then Alternate chunk buffer location
			if (buffer_counter == (ENC_BUFFER_SZ / 2)) {
				// Reset the buffer location counter
				buffer_counter = 0;

				// Toggle offset
				c->buffer_offset = buffer_offset;
				buffer_offset = toggle_offset(buffer_offset);

				s.play_state = REQUEST;
			}

			// Check if shouldn't be playing the song anymore
			if (c->drm_state == STOPPED) {
				set_stopped();
				break;
			}
		}
	}

	mb_printf("Song dump finished\r\n");
	set_stopped();
	return;
}


//Audio output of the encrypted song
void play_encrypted_song(unsigned char *key) {
	struct chachapoly_ctx ctx;
	chachapoly_init(&ctx, key, 256);

	waveHeaderMetaStruct waveHeaderMeta;
	encryptedMetadata metadata;

	int metadata_size = 0;
	int chunks_to_read, chunk_counter = 1;
	int chunk_remainder;

	// Boolean for 30s buffer
	int song_playable = FALSE;
	int song_playable_byte_counter = PREVIEW_SZ;

	// Initialize buffer offset;
	int buffer_offset = 0;
	int buffer_loc = 0;
	int buffer_counter = 0;
	int chunks_decrypted = 0;

	// DMA and fifo variables
	int chunks_copied = 0;
	int bytes_to_play = SONG_CHUNK_SZ;
	int first_time_play = TRUE;

	set_waiting_file_header();

	while (1) {
		while (InterruptProcessed) {
			InterruptProcessed = FALSE;
			set_working();

			switch (c->cmd) {
			case READ_HEADER:
				metadata_size = read_header(&ctx, &waveHeaderMeta);
				if (metadata_size == -1) {
					mb_printf("Song not valid!\r\n");
					return;
				}
				c->metadata_size = metadata_size;

				// Determine how many chunks are going to be read
				// Determine the last chunk size
				chunks_to_read = waveHeaderMeta.wave_header.wav_size / SONG_CHUNK_SZ;
				chunk_remainder = waveHeaderMeta.wave_header.wav_size % SONG_CHUNK_SZ;

				// Start waiting for metadata
				set_waiting_metadata();
				break;
			case READ_METADATA:
				if (read_metadata(&ctx, &metadata) == 0) {
					c->total_chunks = chunks_to_read;
					c->chunk_size = SONG_CHUNK_SZ;
					c->chunk_remainder = chunk_remainder;
					set_waiting_chunk();
					break;
				} else {
					return;
				}
            //Pause, play, restart and stop command handling
			case PAUSE:
				mb_printf("Pausing...\r\n");
				set_paused();
				while(!InterruptProcessed) continue;
				break;
			case PLAY:
				mb_printf("Playing...\r\n");
				set_playing();
				c->cmd = READ_CHUNK;
				break;
			case RESTART:
				mb_printf("Restarting...\r\n");
				set_waiting_file_header();

				usleep(500);

				return;
			case STOP:
				mb_printf("Stopping playback...\r\n");
				return;
			default:
				break;
			}
		}

		// Still in play while loop
		if (c->cmd == READ_CHUNK) {

			// First time run
			if (chunks_decrypted == 0) {
				// Check if any of the song's regions match the player's regions
				for (int i = 0; i < s.purdue_md.num_regions; i++) {
					if (is_provisioned_rid(s.purdue_md.provisioned_regions[i])) {
						// Check to see if logged in user owns the song
						if (s.logged_in && s.uid == s.purdue_md.owner_id) {
							song_playable = TRUE;
						}

						// If the song can be played in the region, check if the logged in user has the song shared to them
						for (int n = 0; n < s.purdue_md.num_users; n++) {
							if (s.uid == s.purdue_md.provisioned_users[n]) {
								song_playable = TRUE;
								break;
							}
						}
					}
				}

				if (song_playable == FALSE) {
					mb_printf("Song is not valid for the region or the user does not have access to this song\r\n");
					mb_printf("Only playing 30s\r\n");
				}

				s.play_state = DECRYPT;
			}

			if (s.play_state == DECRYPT) {
				buffer_loc = buffer_counter++ + ((ENC_BUFFER_SZ / 2) * buffer_offset);

				int chunk_size = SONG_CHUNK_SZ;

				// Check if on the last chunk
				if (chunk_counter == chunks_to_read) {
					chunk_size = chunk_remainder;
				}

				// Read and decrypt the chunk
				if (read_chunks(&ctx, chunk_buffer, s.purdue_md.sha256sum, chunk_size, chunk_counter, buffer_loc) == 0) {
					chunk_counter++;
					chunks_decrypted++;
					s.play_state = COPY;
				} else {
					return;
				}
			}

			if (s.play_state == COPY) {
				// Start playing decrypted chunk
				u32 *fifo_fill = (u32 *) XPAR_FIFO_COUNT_AXI_GPIO_0_BASEADDR;

				int cp_num = (bytes_to_play > CHUNK_SZ) ? CHUNK_SZ : bytes_to_play;
				int offset = (chunks_copied % 2) ? 0 : CHUNK_SZ;

				// Check if on the last chunk
				// This is plus one because it gets increase after decrypting the chunk
				if (chunk_counter + 1 == chunks_to_read) {
					cp_num = chunk_remainder;
				}

				// Check if playing 30seconds
				if (song_playable == FALSE && song_playable_byte_counter <= cp_num) {
					cp_num = song_playable_byte_counter;
				}

				// do first mem cpy here into DMA BRAM
				Xil_MemCpy(
						(void *) (XPAR_MB_DMA_AXI_BRAM_CTRL_0_S_AXI_BASEADDR + offset),
						(void *) (chunk_buffer + SONG_CHUNK_SZ - bytes_to_play),
						(u32) (cp_num));
				// dma_busy will not report correctly the first time
				// Check for first time run, then it should work correctly after
				while (XAxiDma_Busy(&sAxiDma, XAXIDMA_DMA_TO_DEVICE)
						&& !first_time_play
						&& *fifo_fill < (FIFO_CAP - 32)
						) {
				}

				if (first_time_play == TRUE) {
					first_time_play = FALSE;
				}

				fnAudioPlay(sAxiDma, offset, cp_num);

				bytes_to_play -= cp_num;
				song_playable_byte_counter -= cp_num;

				if (bytes_to_play <= 0) {
					bytes_to_play = SONG_CHUNK_SZ;
					chunks_copied++;
					s.play_state = DECRYPT;
				}

				// STOP PLAYBACK
				if (song_playable_byte_counter == 0 && song_playable == FALSE) {
					set_stopped();
					return;
				} else if (chunk_counter + 1 == chunks_to_read) {
					set_stopped();
					return;
				}
			}

			// Request more chunks
			if (s.play_state == REQUEST) {
				set_waiting_chunk();

				// Start decrypting more chunks
				s.play_state = DECRYPT;
			}

			// Check if reached the end of the command buffer
			// Then Alternate chunk buffer location
			if (buffer_counter == (ENC_BUFFER_SZ / 2)) {
				// Reset the buffer location counter
				buffer_counter = 0;

				// Toggle offset
				c->buffer_offset = buffer_offset;
				buffer_offset = toggle_offset(buffer_offset);

				s.play_state = REQUEST;
			}
		}

		// Check if shouldn't be playing the song anymore
		if (c->cmd == STOP) {
			set_stopped();
			break;
		}
	}
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
    memset((void *)c, 0, sizeof(cmd_channel));

    mb_printf("Size of command channel %d", sizeof(cmd_channel));

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
            case QUERY_ENC_SONG:
            	query_enc_song(key);
            	break;
            case ENC_SHARE:
            	share_enc_song(key);
            	break;
            case DIGITAL_OUT:
                digital_out(key);
                break;
            case PLAY_SONG:
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
