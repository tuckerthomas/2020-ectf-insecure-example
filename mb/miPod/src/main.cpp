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

// sends a command to the microblaze using the shared command channel and interrupt
void send_command(int cmd) {
	if (memcpy((void*) &c->cmd, &cmd, 1) == NULL) {
		std::cout << "Could not copy memory: " << (errno) << std::endl;
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
			std::cerr << "More than 3 parameters supplied" << (errno)
					<< std::endl;
			return;
		}
	}

	//std::cout << "cmd: '" << inputs[0] << "' arg1: '" << inputs[1] << "' arg2: '" << inputs[2] << "'" << std::endl;

	cmd = inputs[0];
	arg1 = inputs[1];
	arg2 = inputs[2];
	return;
}

// prints the help message while not in playback
//done
void print_help() {
	//cout is more secure than a printf variant because cout doesn't have format specifiers
	std::cout << "miPod options:\r\n";
	std::cout << "  login<username> <pin>: log on to a miPod account (must be logged out)\r\n";
	std::cout << "  logout: log off a miPod account (must be logged in)\r\n";
	std::cout << "  query <song.drm>: display information about the song\r\n";
	std::cout << "  share <song.drm>: <username>: share the song with the specified user\r\n";
	std::cout << "  play <song.drm>: play the song\r\n";
	std::cout << "  exit: exit miPod\r\n";
	std::cout << "  help: display this message\r\n";
}

// prints the help message while in playback
void print_playback_help() {
	std::cout << "miPod playback options:\r\n";
	std::cout << "  stop: stop playing the song\r\n";
	std::cout << "  pause: pause the song\r\n";
	std::cout << "  resume: resume the paused song\r\n";
	std::cout << "  restart: restart the song\r\n";
	std::cout << "  ff: fast forwards 5 seconds(unsupported)\r\n"; // we probably shouldn't print this
	std::cout << "  rw: rewind 5 seconds (unsupported)\r\n"; // we probably shoudln't print this
	std::cout << "  help: display this message\r\n";
}

FILE *read_enc_file_header(std::string fname) {
	FILE* fd;

	fd = fopen(fname.c_str(), "rb");

	if (fd == NULL) {
		std::cout << "Could not open file! Error = " << (errno) << std::endl;
		return NULL;
	}

	printf("Size of waveheader: %i, Size of enc_wave_header: %i\r\n", sizeof(waveHeaderStruct), sizeof(encryptedWaveheader));

	fread((encryptedWaveheader *) &(c->encWaveHeader), sizeof(encryptedWaveheader), 1, fd);

	send_command(READ_HEADER);
	usleep(500);
	while (c->drm_state == WORKING) continue; // wait for DRM to dump file

	return fd;

}

void read_enc_metadata(FILE *fp, int metadata_size) {
	if (fp == NULL) {
		std::cout << "File error" << std::endl;
		return;
	}

	printf("reading metadata of size %i\r\n", metadata_size);

	int metadata_total_size = NONCE_SIZE + MAC_SIZE + metadata_size;
	unsigned char meta_buffer[metadata_total_size];

	fread(meta_buffer, metadata_total_size, 1, fp);

	memcpy((void *)&(c->encMetadata), meta_buffer, metadata_total_size);

	std::cout << "Last byte: " << c->encMetadata.metadata[METADATA_SZ - 1] << std::endl;

	send_command(READ_METADATA);

	return;
}

void read_enc_chunk(FILE *fp, int chunk_size, int buffer_loc) {
	int chunk_total_size = NONCE_SIZE + MAC_SIZE + chunk_size;

	unsigned char buffer[chunk_total_size];

	fread(buffer, chunk_total_size, 1, fp);

	memcpy((void *)&(c->encSongBuffer[buffer_loc]), buffer, chunk_total_size);

	return;
}

void *read_enc_chunk_thread(void *fp) {
	std::cout << "Start sending chunks!" << std::endl;

	std::cout << "Initialize buffer" << std::endl;
	for (int i = 0; i < ENC_BUFFER_SZ; i++) {
		int chunk_size = c->chunk_size;
		read_enc_chunk((FILE *) fp, chunk_size, i);
	}
	std::cout << "Buffer finished" << std::endl;

	send_command(READ_CHUNK);

	do {
		while(c->drm_state == WAITING_CHUNK) {
			for (int i = 0; i < ENC_BUFFER_SZ / 2; i++) {
				int chunk_size = c->chunk_size;

				// Check for offset
				int buffer_loc = i + ((ENC_BUFFER_SZ / 2) * c->buffer_offset);

				read_enc_chunk((FILE *)fp, chunk_size, buffer_loc);
			}
			std::cout << "Updated buffer" << std::endl;
			send_command(READ_CHUNK);
		}
		while(c->drm_state == READING_CHUNK) continue;
		if (c->drm_state == STOPPED) {
			return 0;
		}
	} while(1);
}

//////////////////////// COMMAND FUNCTIONS ////////////////////////

void login(std::string& username, std::string& pin) {
	//TODO change pin.size() check to the password specified by the rules (5)
	std::cout << "Checking username: '" << username << "' pin: '" << pin << "'" << std::endl;
	if (username.size() == 0 || pin.size() == 0 || username.size() > 8
			|| pin.size() > 10) {
		std::cout << "Invalid user name/PIN\r\n";
		print_help();
		return;
	}
	if (username.find_first_not_of(
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_")
			!= std::string::npos
			|| pin.find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_")
					!= std::string::npos) {
		std::cerr << "Error username/pin not valid\n";
		exit(1); // change this as needed
	}
	//drive DRM
	//instead of strcpy use '=' operator
	username.copy((char *) c->username, USERNAME_SZ, 0);
	pin.copy((char *)c->pin, MAX_PIN_SZ, 0);
	send_command(LOGIN);
}

// logs out for a user
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
    std::cout << "Regions: " << buffer;

    for (int i = 1; i < c->query.num_regions; i++) {
    	buffer = std::string((char *) q_region_lookup(c->query, i));
        std::cout << ", " << buffer;
    }
    std::cout << std::endl;

    std::cout << "Authorized users: ";
    if (c->query.num_users) {
        buffer = std::string((char *)q_user_lookup(c->query, 0));
        std::cout << buffer;
        for (int i = 1; i < c->query.num_users; i++) {
        	buffer = std::string((char *)q_user_lookup(c->query, i));
            std::cout << ", " << buffer;
        }
    }
    std::cout << std::endl;
}

void query_enc_song(std::string song_name) {
	FILE *fd;

	std::cout << "Metadata size should be: " << sizeof(purdue_md) << std::endl;
	std::cout << "Checking for metadata of size: " << METADATA_SZ << std::endl;
	char encryptedMetadataBuffer[ENC_METADATA_SZ];

	// Open the file in read(byte) mode
	fd = fopen(song_name.c_str(), "rb");

	if (fd == NULL) {
		std::cerr << "Could not open " << song_name << " to share:" << (errno) << std::endl;
		return;
	}

	std::cout << "Current position: " << ftell(fd) << std::endl;
	fseek(fd, sizeof(encryptedWaveheader), SEEK_SET);
	std::cout << "Seeked position: " << ftell(fd) << std::endl;

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
	std::cout << "Owner: " << c->query.owner << std::endl;

	std::string buffer((char *)q_region_lookup(c->query, 0));
	std::cout << "Regions: " << buffer;
	for (int i = 1; i < c->query.num_regions; i++) {
		buffer = std::string((char *)q_region_lookup(c->query, i));
		std::cout << ", " << buffer;
	}
	std::cout << std::endl;

	buffer = std::string((char *)c->query.owner);
	std::cout << "Owner: " << buffer << std::endl;

	std::cout << "Authorized users: ";
	if (c->query.num_users) {
		buffer = std::string((char *)q_user_lookup(c->query, 0));
		std::cout << buffer;
		for (int i = 1; i < c->query.num_users; i++) {
			buffer = std::string((char *)q_user_lookup(c->query, i));
			std::cout << ", " << buffer;
		}
	}
	std::cout << std::endl;
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

	std::cout << "Saving to " << song_name_dout << std::endl;

	// Open pointer for digital out file
	FILE *wfp = fopen(song_name_dout.c_str(), "wb");

	if (wfp == NULL) {
		std::cout << "Could not open file! Error = " << (errno) << std::endl;
		return;
	}

	// Open pointer for encrypted file
	FILE *rfp;

	if (c->drm_state == WAITING_FILE_HEADER) {
		// load file into shared buffer
		rfp = read_enc_file_header(song_name);
	}

	if (rfp == NULL) {
		std::cout << "Could not read file" << std::endl;
		return;
	}

	// Wait for new command;
	while (c->drm_state == STOPPED) continue;
	while (c->drm_state == WORKING) continue;

	if (c->drm_state == WAITING_METADATA) {
		// Copy decrypted metadata to new file
		fwrite((unsigned char *)c->wav_header, WAVE_HEADER_SZ, 1, wfp);

		std::cout << "Start reading metadata!" << std::endl;
		int metadata_size = c->metadata_size;
		read_enc_metadata(rfp, metadata_size);
		std::cout << "Metadata read!" << std::endl;
	}

	std::cout << "Waiting for metadata to process" << std::endl;
	while (c->drm_state == WAITING_METADATA) {
		continue;
	}

	send_command(WAIT_FOR_CHUNK);

	std::cout << "Metadata Processed!" << std::endl;

	std::cout << "Initialize buffer" << std::endl;
	for (int i = 0; i < ENC_BUFFER_SZ; i++) {
		int chunk_size = c->chunk_size;
		read_enc_chunk(rfp, chunk_size, i);
	}
	std::cout << "Buffer finished" << std::endl;
	send_command(READ_CHUNK);

	int total_chunks_written = 0;

	while (1) {
		while (c->drm_state == WAITING_CHUNK) {
			std::cout << "Updating buffer" << std::endl;
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

			std::cout << "Updated buffer" << std::endl;
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
					std::cout << "Writing last chunk!" << std::endl;
					fwrite((unsigned char *) &c->songBuffer[SONG_CHUNK_SZ * buffer_loc], c->chunk_remainder, 1, wfp);
				} else {
					fwrite((unsigned char *) &c->songBuffer[SONG_CHUNK_SZ * buffer_loc], SONG_CHUNK_SZ, 1, wfp);
				}

				total_chunks_written++;
			}
			break;
		}
	}

	std::cout << "Song dump finished" << std::endl;
	std::cout << "Wrote " << total_chunks_written << " number of chunks" << std::endl;
	fclose(wfp);
	fclose(rfp);
	return;

}

// attempts to share a song with a user
void share_enc_song(std::string& song_name, std::string& username) {
	FILE *fd;
	unsigned int length;

	if (username.empty()) {
		std::cout << "Need song name and username\r\n";
		print_help();
		return;
	}

	// Create a buffer for the read metadata
	char encryptedMetadataBuffer[ENC_METADATA_SZ];

	// Open the file in read(byte) mode
	fd = fopen(song_name.c_str(), "rb");

	if (fd == NULL) {
		std::cerr << "Could not open " << song_name << " to share:" << (errno) << std::endl;
		return;
	}

	std::cout << "Starting at " << ftell(fd) << std::endl;
	fseek(fd, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, SEEK_SET);
	std::cout << "Seeked to " << ftell(fd) << std::endl;

	// Read the encrypted metadata into the buffer
	fread(encryptedMetadataBuffer, ENC_METADATA_SZ, 1, fd);

	//fclose(fd);

	//fd = NULL;

	// Copy the local buffer to the command buffer
	memcpy((encryptedMetadata *)&c->encMetadata, encryptedMetadataBuffer, ENC_METADATA_SZ);

	// Check username argument
	if (username.empty()) {
		std::cout << "Need song name and username\r\n";
		print_help();
	}

	username.copy((char *) c->username, USERNAME_SZ, 0);

	// drive DRM
	send_command(ENC_SHARE);
	while (c->drm_state == STOPPED) continue; // wait for DRM to start working
	while (c->drm_state == WORKING) continue; // wait for DRM to start working

	// TODO: Change how failed encrypted metadata is checked
	// request was rejected if WAV length is 0
	length = c->song.wav_size;
	if (length == 0) {
		std::cerr << "Share rejected\r\n";
		return;
	}

	fseek(fd, 0, SEEK_END);
	int endFileSZ = ftell(fd);
	fseek(fd, 0, SEEK_SET);

	// open output file
	std::string temp_song_name = song_name;
	temp_song_name.append(".temp");

	FILE* fd2;
	fd2 = fopen(temp_song_name.c_str(), "wb");

	if (fd2 == NULL) {
		std::cerr << "Failed to open file! Error = " << (errno) << "\r\n";
		return;
	}

	// Max size it will be, ENC_METADATA_SZ
	char buffer[ENC_METADATA_SZ];

	// Read WAVE_HEADER
	fread(buffer, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, 1, fd);

	// Write WAVE_HEADER
	fwrite(buffer, ENC_WAVE_HEADER_SZ + META_DATA_ALLOC, 1, fd2);

	fseek(fd, ENC_METADATA_SZ, SEEK_CUR);

	// Write new metadata
	fwrite((encryptedMetadata *)&c->encMetadata, ENC_METADATA_SZ, 1, fd2);

	static unsigned char song_buffer[MAX_SONG_SZ];

	int byte_to_read = endFileSZ - (ENC_WAVE_HEADER_SZ + META_DATA_ALLOC + ENC_METADATA_SZ);
	std::cout << "Size of song_buffer: " << sizeof(song_buffer) << std::endl;
	std::cout << "file size: " << endFileSZ << std::endl;
	std::cout << "Bytes to read: " << byte_to_read << std::endl;
 	fread(song_buffer, byte_to_read, 1, fd);
	fwrite(song_buffer, byte_to_read, 1, fd2);

	fclose(fd);
	fclose(fd2);

	// Delete old song file and rename new

	if ( remove( song_name.c_str() ) != 0 ){
		std::cerr << "Failed to remove file! Error = " << (errno) << "\r\n";
		return;
	}

	std::cout << song_name << std::endl;

	if ( rename( temp_song_name.c_str() , song_name.c_str() ) != 0 ){
		std::cerr << "Failed to rename file! Error = " << (errno) << "\r\n";
		return;
	}

	std::cout << "Finished writing file\r\n";
}

void play_encrypted_song(std::string song_name) {

	std::cout << "Playing Encrypted Song" << std::endl;

	//char usr_cmd[USR_CMD_SZ + 1], *cmd = NULL, *arg1 = NULL, *arg2 = NULL;
	std::string usr_cmd = "";
	std::string cmd;
	std::string arg1 = "";
	std::string arg2 = "";

	// load song into shared buffer
	FILE *fp = read_enc_file_header(song_name);
	if (fp == NULL) {
		return;
	}

	while (c->drm_state == STOPPED) continue;
	while (c->drm_state == WORKING) continue;

	if (c->drm_state == WAITING_METADATA) {
		std::cout << "Start reading metadata!" << std::endl;
		int metadata_size = c->metadata_size;
		read_enc_metadata(fp, metadata_size);
	}

	std::cout << "Waiting for metadata to process" << std::endl;
	while (c->drm_state == WAITING_METADATA) {
		continue;
	}
	std::cout << "Metadata Processed!" << std::endl;
	while (c->drm_state == STOPPED) continue;
	while (c->drm_state == WORKING) continue;

	std::cout << "Check if we can send a song chunk" << std::endl;
	if (c->drm_state == WAITING_CHUNK) {

		// Start Thread!

		pthread_t chunk_read_thread;

		pthread_create(&chunk_read_thread, NULL, read_enc_chunk_thread, fp);
	}

	// play loop
	while (1) {
		// get a valid command
		do {
			print_prompt_msg(song_name.c_str());
			std::getline(std::cin, usr_cmd);

			// exit playback loop if DRM has finished song
			if (c->drm_state == STOPPED) {
				std::cout << "Song finished\r\n";
				return;
			}
		} while (usr_cmd.length() < 2); //chars are one byte so this is fine

		std::cout << "Checking command: " << cmd << std::endl;

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
				std::cout << "Exiting...\r\n";
				send_command(STOP);
				return;
			} else if (cmd == "rw") {
				mp_printf("Unsupported feature.\r\n\r\n");
				print_playback_help();
			} else if (cmd == "ff") {
				std::cout << "Unsupported feature.\r\n\r\n";
				print_playback_help();
			} else if (cmd == "lyrics") {
				std::cout << "Unsupported feature.\r\n\r\n";
				print_playback_help();
			} else {
				std::cout << "Unrecognized command." << std::endl;
				print_playback_help();
			}
		} else {
			std::cout << "Please enter a command." << std::endl;
			print_playback_help();
		}
	}

	return;
}

//////////////////////// MAIN ////////////////////////

int main(int argc, char** argv) {
	std::cout << "Entering Main Loop" << std::endl;
	int mem;
	// char usr_cmd[USR_CMD_SZ + 1], *cmd = NULL, *arg1 = NULL, *arg2 = NULL;
	// memset(usr_cmd, 0, USR_CMD_SZ + 1);
	std::string usr_cmd = "";
	std::string cmd;
	std::string arg1 = "";
	std::string arg2 = "";

	// Check cmd_channel size
	std::cout << "CMD_CHANNEL SIZE: " << sizeof(cmd_channel) << std::endl;

	// open command channel
	mem = open("/dev/uio0", O_RDWR);
	c = (cmd_channel*) mmap(NULL, sizeof(cmd_channel), PROT_READ | PROT_WRITE,
			MAP_SHARED, mem, 0);
	if (c == MAP_FAILED) {
		std::cerr << "MMAP Failed! Error = " << (errno);
		return -1;
	}

	// dump player information before command loop
	query_player();

	// go into command loop until exit is requested
	while (1) {
		// get command
		print_prompt();
		//fgets(usr_cmd, USR_CMD_SZ, stdin);
		std::getline(std::cin, usr_cmd);

		// parse and handle command
		parse_input(usr_cmd, cmd, arg1, arg2);

		std::cout << "Checking command: '" << cmd << "'" << std::endl;

		if (!cmd.empty()) {
			if (cmd == "help") {
				print_help();
			} else if (cmd == "login") {
				login(arg1, arg2);
			} else if (cmd == "logout") {
				logout();
			} else if (cmd == "query_enc") {
				query_enc_song(arg1);
			} else if (cmd == "digital_out") {
				digital_out(arg1);
			} else if (cmd == "share_enc") {
				share_enc_song(arg1, arg2);
			} else if (cmd == "play_enc_song") {
				play_encrypted_song(arg1);
			} else if (cmd == "exit") {
				std::cout << "Exiting..." << std::endl;
				break;
			} else {
				std::cout << "Unrecognized command." << std::endl;
				print_help();
			}
		} else {
			std::cout << "Please enter a command." << std::endl;
			print_help();
		}
	}

	// unmap the command channel
	munmap((void*) c, sizeof(cmd_channel));

	return 0;
}
