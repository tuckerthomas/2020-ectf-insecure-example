/*
 * eCTF Collegiate 2020 miPod Example Code
 * Linux-side DRM driver
 */

#include "miPodCpp.h"

#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <linux/gpio.h>
#include <string.h>
#include <pthread.h>

//c++ includes
#include <iostream>
#include <fstream>
#include <sstream>
#include <bits/stdc++.h>
#include <string>

//change headerfile so that all structs use std::string instead of char[] or char*
volatile cmd_channel *c;

//////////////////////// UTILITY FUNCTIONS ////////////////////////

template<typename ...Args>
void mp_print(Args && ...args) {
	(std::cout << "mP> " << ... << args);
}

// sends a command to the microblaze using the shared command channel and interrupt
void send_command(int cmd) {
	if (memcpy((void*) &c->cmd, &cmd, 1) == NULL) {
		mp_print("Could not copy memory: ", (errno), "\r\n");
	}

	//trigger gpio interrupt
	system("devmem 0x41200000 32 0"); //reconsider the use of the system command
	system("devmem 0x41200000 32 1"); //reconsider the use of the system command
}

// parses the input of a command with up to two arguments
void parse_input(std::string input, std::string& cmd, std::string& arg1,
		std::string& arg2) {

	std::string inputs[3];

	std::stringstream ssin(input);

	int i = 0;

	while (ssin.good()) {
		if (i < 3) {
			ssin >> inputs[i++];
		} else {
			mp_print("More than 3 parameters supplied", (errno), "\r\n");
			return;
		}
	}

	cmd = inputs[0];
	arg1 = inputs[1];
	arg2 = inputs[2];
	return;
}

// prints the help message while not in playback
void print_help() {
	mp_print("miPod options:\r\n",
			"  login<username> <pin>: log on to a miPod account (must be logged out)\r\n",
			"  logout: log off a miPod account (must be logged in)\r\n",
			"  query <song.drm>: display information about the song\r\n",
			"  share <song.drm>: <username>: share the song with the specified user\r\n",
			"  play <song.drm>: play the song\r\n",
			"  exit: exit miPod\r\n",
			"  help display this message\r\n");
}

// prints the help message while in playback
void print_playback_help() {
	mp_printf("miPod playback options:\r\n",
			"  stop: stop playing the song\r\n",
			"  pause: pause the song\r\n",
			"  resume: resume the paused song\r\n",
			"  restart: restart the song\r\n",
			"  ff: fast forwards 5 seconds(unsupported)\r\n",
			"  rw: rewind 5 seconds (unsupported)\r\n",
			"  help: display this message\r\n");
}

FILE *read_enc_file_header(std::string fname) {
	FILE* fd;

	fd = fopen(fname.c_str(), "rb");

	if (fd == NULL) {
		mp_print("Could not open file! Error = ", (errno), "\r\n");
		return NULL;
	}

	fread((encryptedWaveheader *) &(c->encWaveHeader), sizeof(encryptedWaveheader), 1, fd);

	send_command(READ_HEADER);
	usleep(500);
	while (c->drm_state == WORKING) continue; // wait for DRM to dump file

	return fd;

}

//Reads song metadata chunk by chunk and stores it in the file stream
void read_enc_metadata(FILE *fp, int metadata_size) {
	if (fp == NULL) {
		mp_print("File error\r\n");
		return;
	}

	int metadata_total_size = NONCE_SIZE + MAC_SIZE + metadata_size;
	unsigned char meta_buffer[metadata_total_size];

	fread(meta_buffer, metadata_total_size, 1, fp);

	memcpy((void *)&(c->encMetadata), meta_buffer, metadata_total_size);

	send_command(READ_METADATA);

	return;
}

//Reads encrypted song chunk
void read_enc_chunk(FILE *fp, int chunk_size, int buffer_loc) {
	int chunk_total_size = NONCE_SIZE + MAC_SIZE + chunk_size;

	unsigned char buffer[chunk_total_size];

	fread(buffer, chunk_total_size, 1, fp);

	memcpy((void *)&(c->encSongBuffer[buffer_loc]), buffer, chunk_total_size);

	return;
}

//New thread for requesting and decrypting chunks
void *decryption_thread(void *song_name) {
	mp_print("Starting decryption thread!\r\n");

	// handle restart
	do {
		send_command(PLAY_SONG);

		while (c->drm_state == STOPPED)
			continue; // wait for DRM to start working
		while (c->drm_state == WORKING)
			continue; // wait for DRM to dump file

		FILE *fp;

		if (c->drm_state == WAITING_FILE_HEADER) {
			// load file into shared buffer
			mp_print("Opening ", (char *) song_name, "\r\n");
			fp = read_enc_file_header((char *) song_name);
		}

		if (fp == NULL) {
			mp_print("Could not open file\r\n");
			return (void *) -1;
		}

		// Wait for new command;
		while (c->drm_state == STOPPED)
			continue;
		while (c->drm_state == WORKING)
			continue;

		if (c->drm_state == WAITING_METADATA) {
			int metadata_size = c->metadata_size;
			read_enc_metadata(fp, metadata_size);
		}

		while (c->drm_state == WAITING_METADATA) {
			continue;
		}

		send_command(WAIT_FOR_CHUNK);

		// Initialize a buffer before playing
		for (int i = 0; i < ENC_BUFFER_SZ; i++) {
			int chunk_size = c->chunk_size;
			read_enc_chunk(fp, chunk_size, i);
		}
		send_command(READ_CHUNK);

		while (1) {
			if (c->drm_state == WAITING_CHUNK) {
				// Read encrypted chunks from rfp
				for (int i = 0; i < ENC_BUFFER_SZ / 2; i++) {
					int chunk_size = c->chunk_size;

					// Check for offset
					int buffer_loc = i
							+ ((ENC_BUFFER_SZ / 2) * c->buffer_offset);

					read_enc_chunk(fp, chunk_size, buffer_loc);
				}

				c->drm_state = READING_CHUNK;
				usleep(500);
			}

			// Song playback stopped
			if (c->drm_state == STOPPED) {
				fclose(fp);
				break;
			}

			// Restarting playback
			if (c->drm_state == WAITING_FILE_HEADER) {
				fclose(fp);
				break;
			}
		}
	} while (c->drm_state != STOPPED);

	mp_print("Leaving decryption thread!\r\n");

	return (void *) 0;
}

//////////////////////// COMMAND FUNCTIONS ////////////////////////

//Allows for a user to login to microblaze with username and pin
void login(std::string& username, std::string& pin) {
	//TODO change pin.size() check to the password specified by the rules (5)

	if (username.size() == 0 || pin.size() == 0 || username.size() > USERNAME_SZ
			|| pin.size() > MAX_PIN_SZ) {
		mp_print("Invalid user name/PIN\r\n");
		print_help();
		return;
	}
	if (username.find_first_not_of(
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_")
			!= std::string::npos
			|| pin.find_first_not_of(
					"01234567890")
					!= std::string::npos) {
		mp_print("Error username/pin not valid\r\n");
		return;
	}

	strncpy((char *) c->username, username.c_str(), USERNAME_SZ);
	strncpy((char *) c->pin, pin.c_str(), MAX_PIN_SZ);

	send_command(LOGIN);
    while (c->drm_state == STOPPED) continue; // wait for DRM to start working
    while (c->drm_state == WORKING) continue; // wait for DRM to dump file
}

// logsout the current logged in user
void logout() {
	// drive DRM
	send_command(LOGOUT);
}

// queries the DRM about the player
// DRM will fill shared buffer with query content
void query_player() {
	// drive DRM
	send_command(QUERY_PLAYER);
    while (c->drm_state == STOPPED) continue; // wait for DRM to start working
    while (c->drm_state == WORKING) continue; // wait for DRM to dump file

    // print query results
    std::string buffer((char *) q_region_lookup(c->query, 0));
    mp_print( "Regions: " , buffer);

    for (unsigned int i = 1; i < c->query.num_regions; i++) {
    	buffer = std::string((char *) q_region_lookup(c->query, i));
    	std::cout << ", " << buffer;
    }
    std::cout << "\r\n";

    mp_print( "Authorized users: ");
    if (c->query.num_users) {
        buffer = std::string((char *)q_user_lookup(c->query, 0));
        std::cout << buffer;
        for (unsigned int i = 1; i < c->query.num_users; i++) {
        	buffer = std::string((char *)q_user_lookup(c->query, i));
            std::cout << ", " << buffer;
        }
    }
    std::cout << "\r\n";
}

//Queries metadata of encrypted song and prints information from metadata
void query_enc_song(std::string song_name) {
	FILE *fd;

	char encryptedMetadataBuffer[ENC_METADATA_SZ];

	// Open the file in read(byte) mode
	fd = fopen(song_name.c_str(), "rb");

	if (fd == NULL) {
		mp_print("Could not open " , song_name , " to share:" , (errno) , "\r\n");
		return;
	}

	// Seek past the wave header
	fseek(fd, sizeof(encryptedWaveheader), SEEK_SET);

	// Read the encrypted metadata into the buffer
	fread(encryptedMetadataBuffer, ENC_METADATA_SZ, 1, fd);

	memcpy((encryptedMetadata *)&c->encMetadata, encryptedMetadataBuffer, ENC_METADATA_SZ);

	// drive DRM
	send_command(QUERY_ENC_SONG);
	while (c->drm_state == STOPPED) {
		continue;
	}
	while (c->drm_state == WORKING) {
		continue; // wait for DRM to finish
	}

	// print query results
	mp_print( "Owner: " , (unsigned char *) c->query.owner , "\r\n");

	std::string buffer((char *)q_region_lookup(c->query, 0));

	mp_print( "Regions: " , buffer);

	for (unsigned int i = 1; i < c->query.num_regions; i++) {
		buffer = std::string((char *)q_region_lookup(c->query, i));
		std::cout << ", " << buffer;
	}
	std::cout << "\r\n";

	buffer = std::string((char *)c->query.owner);
	mp_print( "Owner: " , buffer , "\r\n");

	mp_print( "Authorized users: ");
	if (c->query.num_users) {
		buffer = std::string((char *)q_user_lookup(c->query, 0));
		std::cout << buffer;
		for (unsigned int i = 1; i < c->query.num_users; i++) {
			buffer = std::string((char *)q_user_lookup(c->query, i));
			std::cout << ", " << buffer;
		}
	}
	std::cout << "\r\n";
}

// turns DRM song into original WAV for digital output
void digital_out(std::string song_name) {
	// drive DRM
	send_command(DIGITAL_OUT);

	while (c->drm_state == STOPPED)
		continue; // wait for DRM to start working
	while (c->drm_state == WORKING)
		continue; // wait for DRM to dump file

	std::string song_name_dout = song_name;
	song_name_dout.append(".dout");

	mp_print( "Saving to " , song_name_dout , "\r\n");

	// Open pointer for digital out file
	FILE *wfp = fopen(song_name_dout.c_str(), "wb");

	if (wfp == NULL) {
		mp_print( "Could not open file! Error = " , (errno) , "\r\n");
		return;
	}

	// Open pointer for encrypted file
	FILE *rfp;

	if (c->drm_state == WAITING_FILE_HEADER) {
		// load file into shared buffer
		rfp = read_enc_file_header(song_name);
	}

	if (rfp == NULL) {
		mp_print( "Could not read file" , "\r\n");
		return;
	}

	// Wait for new command;
	while (c->drm_state == STOPPED) continue;
	while (c->drm_state == WORKING) continue;

	if (c->drm_state == WAITING_METADATA) {
		// Copy decrypted metadata to new file
		fwrite((unsigned char *)c->wav_header, WAVE_HEADER_SZ, 1, wfp);

		int metadata_size = c->metadata_size;
		read_enc_metadata(rfp, metadata_size);
		mp_print( "Metadata read!" , "\r\n");
	}

	while (c->drm_state == WAITING_METADATA) {
		continue;
	}

	send_command(WAIT_FOR_CHUNK);

	// Initialize a buffer before playing
	for (int i = 0; i < ENC_BUFFER_SZ; i++) {
		int chunk_size = c->chunk_size;
		read_enc_chunk(rfp, chunk_size, i);
	}
	send_command(READ_CHUNK);

	int total_chunks_written = 0;

	while (1) {
		while (c->drm_state == WAITING_CHUNK) {
			// Read decrypted chunks from buffer
			for (int i = 0; i < ENC_BUFFER_SZ / 2; i++) {
				int buffer_loc = i + ((ENC_BUFFER_SZ / 2) * c->buffer_offset);
				fwrite((unsigned char *) &c->songBuffer[SONG_CHUNK_SZ * buffer_loc], SONG_CHUNK_SZ, 1, wfp);
				total_chunks_written++;
			}

			// Read encrypted chunks from rfp
			for (int i = 0; i < ENC_BUFFER_SZ / 2; i++) {
				int chunk_size = c->chunk_size;

				// Check for offset
				int buffer_loc = i + ((ENC_BUFFER_SZ / 2) * c->buffer_offset);

				read_enc_chunk(rfp, chunk_size, buffer_loc);
			}

			send_command(READ_CHUNK);
		}

		while (c->drm_state == READING_CHUNK)
			continue;

		if (c->drm_state == STOPPED) {
			// Read out last buffer
			// buffer offset doesn't get toggled before the song finished decrypting its last buffer

			// Account for uneven
			int last_chunks = (c->total_chunks % (ENC_BUFFER_SZ / 2) == 0) ? ENC_BUFFER_SZ / 2 : c->total_chunks % (ENC_BUFFER_SZ / 2);
			for (int i = 0; i < last_chunks; i++) {
				int buffer_loc = i + ((ENC_BUFFER_SZ / 2) * !c->buffer_offset);

				if (i == last_chunks - 1) {
					mp_print( "Writing last chunk!" , "\r\n");
					fwrite((unsigned char *) &c->songBuffer[SONG_CHUNK_SZ * buffer_loc], c->chunk_remainder, 1, wfp);
				} else {
					fwrite((unsigned char *) &c->songBuffer[SONG_CHUNK_SZ * buffer_loc], SONG_CHUNK_SZ, 1, wfp);
				}

				total_chunks_written++;
			}
			break;
		}
	}

	mp_print( "Song dump finished" , "\r\n");

	fclose(wfp);
	fclose(rfp);
	return;

}

// attempts to share a song with a user
void share_enc_song(std::string& song_name, std::string& username) {
	mp_print( "Attempting to share " , song_name , " with " , username , "\r\n");
	FILE *fd;

	if (username.empty()) {
		mp_print( "Need song name and username\r\n");
		print_help();
		return;
	}

	// Create a buffer for the read metadata
	char encryptedMetadataBuffer[ENC_METADATA_SZ];

	// Open the file in read(byte) mode
	fd = fopen(song_name.c_str(), "rb");

	if (fd == NULL) {
		mp_print("Could not open " , song_name , " to share:" , (errno) , "\r\n");
		return;
	}

	fseek(fd, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, SEEK_SET);

	// Read the encrypted metadata into the buffer
	fread(encryptedMetadataBuffer, ENC_METADATA_SZ, 1, fd);

	// Copy the local buffer to the command buffer
	memcpy((encryptedMetadata *)&c->encMetadata, encryptedMetadataBuffer, ENC_METADATA_SZ);

	// Check username argument
	if (username.empty()) {
		mp_print( "Need song name and username\r\n");
		print_help();
	}

	username.copy((char *) c->username, USERNAME_SZ, 0);

	// drive DRM
	send_command(ENC_SHARE);
	while (c->drm_state == STOPPED) continue; // wait for DRM to start working
	while (c->drm_state == WORKING) continue; // wait for DRM to start working

	// Check if the share was rejected
	if (c->share_rejected == 1) {
		mp_print("Share rejected\r\n");
		return;
	}

	// Get file size
	fseek(fd, 0, SEEK_END);
	int endFileSZ = ftell(fd);
	fseek(fd, 0, SEEK_SET);

	// open output file
	std::string temp_song_name = song_name;
	temp_song_name.append(".temp");

	FILE* fd2;
	fd2 = fopen(temp_song_name.c_str(), "wb");

	if (fd2 == NULL) {
		mp_print("Failed to open file! Error = " , (errno) , "\r\n");
		return;
	}

	// Max size it will be, ENC_METADATA_SZ
	char buffer[ENC_METADATA_SZ];

	// Read WAVE_HEADER
	fread(buffer, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, 1, fd);

	// Write WAVE_HEADER
	fwrite(buffer, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, 1, fd2);

	// Seek past metadata
	fseek(fd, ENC_METADATA_SZ, SEEK_CUR);

	// Write new metadata
	fwrite((encryptedMetadata *)&c->encMetadata, ENC_METADATA_SZ, 1, fd2);

	static unsigned char song_buffer[MAX_SONG_SZ];

	int byte_to_read = endFileSZ - (ENC_WAVE_HEADER_SZ + META_DATA_ALLOC + ENC_METADATA_SZ);
	mp_print( "Size of song_buffer: " , sizeof(song_buffer) , "\r\n");
	mp_print( "file size: " , endFileSZ , "\r\n");
	mp_print( "Bytes to read: " , byte_to_read , "\r\n");
 	fread(song_buffer, byte_to_read, 1, fd);
	fwrite(song_buffer, byte_to_read, 1, fd2);

	fclose(fd);
	fclose(fd2);

	// Delete old song file and rename new
	if ( remove( song_name.c_str() ) != 0 ){
		mp_print("Failed to remove file! Error = " , (errno) , "\r\n");
		return;
	}

	if ( rename( temp_song_name.c_str() , song_name.c_str() ) != 0 ){
		mp_print("Failed to rename file! Error = " , (errno) , "\r\n");
		return;
	}

	mp_print( "Finished writing file\r\n");
	return;
}

//Outputs the audio content of the encrypted song
void play_encrypted_song(std::string song_name) {

	mp_print( "Playing Encrypted Song" , "\r\n");

	//char usr_cmd[USR_CMD_SZ + 1], *cmd = NULL, *arg1 = NULL, *arg2 = NULL;
	std::string usr_cmd = "";
	std::string cmd;
	std::string arg1 = "";
	std::string arg2 = "";

	// Start decryption thread
	pthread_t dthread;
	pthread_create(&dthread, NULL, decryption_thread, (void *)song_name.c_str());

	// play loop
	while (1) {
		// get a valid command
		do {
			print_prompt_msg(song_name.c_str());
			std::getline(std::cin, usr_cmd);

			// exit playback loop if DRM has finished song
			if (c->drm_state == STOPPED) {
				mp_print( "Song finished\r\n");

				pthread_join(dthread, NULL);

				return;
			}
		} while (usr_cmd.length() < 2); //chars are one byte so this is fine

		// parse and handle command
		parse_input(usr_cmd, cmd, arg1, arg2);

		if (!cmd.empty()) {
			if (cmd == "help") {
				print_playback_help();
			} else if (cmd == "resume") {
				send_command(PLAY);
				usleep(200000); // wait for DRM to print
			} else if (cmd == "pause") {
				send_command(PAUSE);
				usleep(200000); // wait for DRM to print
			} else if (cmd == "stop") {
				send_command(STOP);
				usleep(200000); // wait for DRM to print
				break;
			} else if (cmd == "restart") {
				send_command(RESTART);
			} else if (cmd == "exit") {
				mp_print( "Exiting...\r\n");
				send_command(STOP);
				return;
			} else if (cmd == "rw") {
				mp_printf("Unsupported feature.r\n");
				print_playback_help();
			} else if (cmd == "ff") {
				mp_print( "Unsupported feature.r\n");
				print_playback_help();
			} else {
				mp_print( "Unrecognized command." , "\r\n");
				print_playback_help();
			}
		} else {
			mp_print( "Please enter a command." , "\r\n");
			print_playback_help();
		}

	}

	return;
}

//////////////////////// MAIN ////////////////////////

int main(int argc, char** argv) {
	int mem;

	std::string usr_cmd = "";
	std::string cmd;
	std::string arg1 = "";
	std::string arg2 = "";


	// open command channel
	mem = open("/dev/uio0", O_RDWR);
	c = (cmd_channel*) mmap(NULL, sizeof(cmd_channel), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
	if (c == MAP_FAILED) {
		mp_print("MMAP Failed! Error = " , (errno));
		return -1;
	}

	// dump player information before command loop
	query_player();

	// go into command loop until exit is requested
	while (1) {
		// get command
		print_prompt();
		std::getline(std::cin, usr_cmd);

		// parse and handle command
		parse_input(usr_cmd, cmd, arg1, arg2);

		if (!cmd.empty()) {
			if (cmd == "help") {
				print_help();
			} else if (cmd == "login") {
				login(arg1, arg2);
			} else if (cmd == "logout") {
				logout();
			} else if (cmd == "query") {
				query_enc_song(arg1);
			} else if (cmd == "digital_out") {
				digital_out(arg1);
			} else if (cmd == "share") {
				share_enc_song(arg1, arg2);
			} else if (cmd == "play") {
				play_encrypted_song(arg1);
			} else if (cmd == "exit") {
				mp_print( "Exiting..." , "\r\n");
				break;
			} else {
				mp_print( "Unrecognized command." , "\r\n");
				print_help();
			}
		} else {
			mp_print( "Please enter a command." , "\r\n");
			print_help();
		}
	}

	// unmap the command channel
	munmap((void*) c, sizeof(cmd_channel));

	return 0;
}
