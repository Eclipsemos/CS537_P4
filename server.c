#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include "ufs.h"
#include "udp.h"
#include "mfs.h"
#include "message.h"

#define BLOCKSIZE (4096)

struct sockaddr_in client_addr;

void* head;
static int PORTNUM;

static super_t* SUPERBLOCKPTR;
int sd;
int fd;
void *image;

void* in_bm_addr; 
void* d_bm_addr; 
void* in_rn_addr; 
void* d_rn_addr; 

typedef struct {
	inode_t inodes[UFS_BLOCK_SIZE / sizeof(inode_t)];
} inode_block_t;

typedef struct {
	dir_ent_t entries[128];
} dir_block_t;

typedef struct {
	unsigned int bits[UFS_BLOCK_SIZE / sizeof(unsigned int)];
} bitmap_t;

void intHandler(int dummy) {
    UDP_Close(sd);
    exit(130);
}


unsigned int get_bit(unsigned int *bitmap, int position) {
   int index = position / 32;
   int offset = 31 - (position % 32);
   return (bitmap[index] >> offset) & 0x1;
}

int get_new_inum()
{
	for(int i = 0; i < SUPERBLOCKPTR->num_inodes; i++){
        int bit = get_bit(in_bm_addr, i);
		if(bit==0)
		{
			return i;
		}
    	}
	return -1;
}

void set_bit(unsigned int *bitmap, int position) {
   int index = position / 32;
   int offset = 31 - (position % 32);
   bitmap[index] |= 0x1 << offset;
}

int server_shutdown(message_t* m){
	fsync(fd);
	close(fd);
	UDP_Close(PORTNUM);
	exit(0);
	return 0;
}


int server_creat(int pinum, int type, char *name)
{
	// LOOK UP PROCESS FOR THE PINUM //
	inode_t* inode_parent = in_rn_addr + pinum *sizeof(inode_t);// get the parent inode
	int size = inode_parent->size;
	int entry_num = (UFS_BLOCK_SIZE / sizeof(dir_ent_t)) * (size / UFS_BLOCK_SIZE) + ((size % UFS_BLOCK_SIZE) / sizeof(dir_ent_t));
	int num_block = size / (UFS_BLOCK_SIZE + 1) + 1;
	int data_blocks[num_block]; // stores the blocks' addr (in blocks)
    dir_ent_t* entries[entry_num];
    for(int i = 0; i < num_block; i++)
	{
        data_blocks[i] = inode_parent->direct[i];
    }

	int remain_entry = entry_num;
	for(int i=0;i<num_block;i++)
	{
		int curr_data_block = data_blocks[i];
		unsigned long curr_data_block_offset = curr_data_block * UFS_BLOCK_SIZE;
		unsigned long num_loop = (remain_entry < 32)? remain_entry:32;
		void* curr_data_addr = (void*)((unsigned long)curr_data_block_offset+(unsigned long)head);
		for(int j=0;j<num_loop;j++)
		{
			entries[j+32*i] = (dir_ent_t*)curr_data_addr;
			remain_entry--;
			curr_data_addr = (void*)curr_data_addr; // real address
			curr_data_addr += sizeof(dir_ent_t);
		}
	}
	int found_name = 0;
	
	for(int i=0;i<entry_num;i++)
	{
		if(!strcmp(name, entries[i]->name))
		{
			found_name = 1;
			break;
		}
	}

	if(found_name==1)
	{
		printf("Server::create:: ALEADY EXISTED\n");
		message_t msg;
		msg.type = MFS_CRET;
		msg.rc = 1;
		UDP_Write(sd,&client_addr,(char*)&msg,sizeof(message_t));
		return 0; //found the success
	}
	
	// END LOOK UP //
	inode_t *parent = in_rn_addr + pinum * sizeof(inode_t);
	int parent_type = parent->type;
	if(parent_type!=MFS_DIRECTORY)
	{
		message_t send_back;
		send_back.rc = -1;
		send_back.type = MFS_CRET;
		UDP_Write(sd,&client_addr,(char*)&send_back,sizeof(message_t));
		///fix me
		return -1;
	}



	//find the place to add
	int next_free_block = 0;
	while((int)parent->direct[next_free_block]!=-1)
	{
		next_free_block++;
	}
	next_free_block--;
	
	int entry_left = parent->size/sizeof(dir_ent_t);
	unsigned long dir_block = parent->direct[next_free_block];
    unsigned long block_offset = dir_block * UFS_BLOCK_SIZE;

	//dir_ent_t* entries[entry_left];
	void* dir_block_addr = (void*)(block_offset+(unsigned long)head);
	int j=0;
	for( j=0;j<entry_left;j++)
	{
		entries[j] = (dir_ent_t*)dir_block_addr;
        dir_block_addr = (void*)dir_block_addr; 
        dir_block_addr += sizeof(dir_ent_t);
	}
	j-=1;
	entries[entry_left] = (dir_ent_t*) dir_block_addr;

	if(sizeof(entries)<(4096-32))
	{
		strcpy(entries[entry_left]->name,name);	
		entries[entry_left]->inum = get_new_inum();
	}

	if(sizeof(entries)>=(4096-32))
	{
		unsigned long new_block;
        unsigned long new_block_addr;
        int k;
        for(k = 0; k < SUPERBLOCKPTR->num_data; k++){
            if(get_bit(d_bm_addr, j)) continue; // jth data block is used, continue find
            // use jth data block
            new_block = k + SUPERBLOCKPTR->data_region_addr;
            new_block_addr = new_block * UFS_BLOCK_SIZE;
            break;                   
        }
        entries[remain_entry] = (dir_ent_t*)(void*)(unsigned long)new_block_addr;
        strcpy(entries[k+1]->name, name);
        entries[remain_entry]->inum = get_new_inum();;
        set_bit(in_bm_addr, get_new_inum());
	}
	parent->size +=sizeof(dir_ent_t);
	int inode_num;

	if(type == MFS_REGULAR_FILE){
        // add new inode
        inode_num = get_new_inum();
        
        inode_t* new_inode = in_rn_addr + inode_num * sizeof(inode_t);
        // type
        new_inode->type = MFS_REGULAR_FILE;
        // size
        new_inode->size = 0;
        for(int i = 0; i < 30; i++){
            new_inode->direct[i] = -1;
        }
        set_bit(in_bm_addr, inode_num);
		
    }

	 if(type == MFS_DIRECTORY){
        // add new inode
        inode_num = get_new_inum();
        inode_t* new_inode = in_rn_addr + inode_num * sizeof(inode_t);

        // type
        new_inode->type = MFS_DIRECTORY;

        // size -> 64: write in contains . and ..
        new_inode->size = sizeof(dir_ent_t)*2;

		int ans = -1;
		for(int i = 0; i < SUPERBLOCKPTR->num_data; i++)
		{
        int bit = get_bit(d_bm_addr, i);
        if(bit == 0)
		{
			ans = i;
			break;
		}
		}

		int data_num = ans;
        void* data_addr_ptr = (void*)(unsigned long)(d_rn_addr + data_num * MFS_BLOCK_SIZE); // TODO: Check this
        // write in . and .. in data region
        dir_ent_t* entry1 = (dir_ent_t*)data_addr_ptr;
        entry1->inum = get_new_inum();
        strcpy(entry1->name, ".");
        void* data_addr_ptr2 = (void*)(unsigned long)(d_rn_addr + data_num * MFS_BLOCK_SIZE + sizeof(dir_ent_t)); // TODO: Check this
        dir_ent_t* entry2 = (dir_ent_t*)data_addr_ptr2;
        entry2->inum = pinum;
        strcpy(entry2->name, "..");

        // dir
        new_inode->direct[0] = data_num + SUPERBLOCKPTR->data_region_addr;
        for(int i = 1; i < 30; i++){
            new_inode->direct[i] = -1;
        }
        // update inode and data bitmap
        set_bit(in_bm_addr, inode_num);
        set_bit(d_bm_addr, data_num);
    }

	fprintf(stderr, "DEBUG server creat:: inode_num: %d\n", inode_num);
	message_t send_back;
	send_back.type = MFS_CRET;
	send_back.inum = inode_num;
	send_back.rc = 0;
	UDP_Write(sd, &client_addr, (char*)&send_back, sizeof(message_t));
	return 0;
}

int server_lookup(int pinum, char *name)
{
	inode_t* inode_parent = in_rn_addr + pinum *sizeof(inode_t);// get the parent inode
	int size = inode_parent->size;
	int entry_num = (UFS_BLOCK_SIZE / sizeof(dir_ent_t)) * (size / UFS_BLOCK_SIZE) + ((size % UFS_BLOCK_SIZE) / sizeof(dir_ent_t));
	int num_block = size / (UFS_BLOCK_SIZE + 1) + 1;
	int data_blocks[num_block]; // stores the blocks' addr (in blocks)
    dir_ent_t* entries[entry_num];
    for(int i = 0; i < num_block; i++)
	{
        data_blocks[i] = inode_parent->direct[i];
    }
	int remain_entry = entry_num;
	for(int i=0;i<num_block;i++)
	{
		int curr_data_block = data_blocks[i];
		unsigned long curr_data_block_offset = curr_data_block * UFS_BLOCK_SIZE;
		unsigned long num_loop = (remain_entry < 32)? remain_entry:32;
		void* curr_data_addr = (void*)((unsigned long)curr_data_block_offset+(unsigned long)head);
		for(int j=0;j<num_loop;j++)
		{
			entries[j+32*i] = (dir_ent_t*)curr_data_addr;
			remain_entry--;
			curr_data_addr = (void*)curr_data_addr; // real address
			curr_data_addr += sizeof(dir_ent_t);
		}
	}
	int found_name = 0;
	void* result;
	for(int i=0;i<entry_num;i++)
	{
		
		if(!strcmp(name, entries[i]->name))
		{
			found_name = 1;
			
			result = (void*)entries[i];
			break;
		}
	}

	message_t send_back;
	send_back.type = MFS_LOOKUP;
	//printf("Server::lookup::found number is: %d\n",found_name);
	if(found_name==0)
	{
		send_back.inum = -1;
		UDP_Write(sd,&client_addr,(void*)&send_back,sizeof(message_t));
		return -1; //Look up failed
	}
	
	send_back.inum = ((dir_ent_t*)result)->inum;
	printf("Server::lookup::sendback inum is: %d\n",send_back.inum);
	
	UDP_Write(sd, &client_addr,(char*)&send_back,sizeof(message_t));
	return 0;
}



int server_write(int inum, char *buffer, int offset, int nbytes)
{
	return 0;
}


int message_parser(message_t* msg){
	int message_func = msg->mtype;
	int rc;

	if(message_func == MFS_INIT){
		return 0;
	}else if(message_func == MFS_LOOKUP){
		rc = server_lookup(msg->pinum,msg->name);

	}else if(message_func == MFS_STAT){
		//rc = run_stat(m);

	}else if(message_func == MFS_WRITE){
		rc = server_write(msg->inum,msg->name,msg->offset,msg->nbytes);

	}else if(message_func == MFS_READ){
		//rc = run_read(m);

	}else if(message_func == MFS_CRET){
		rc = server_creat(msg->pinum,msg->type,msg->name);

	}else if(message_func == MFS_UNLINK){
		//rc = run_unlink(m);

	}else if(message_func == MFS_SHUTDOWN){
		rc = server_shutdown(msg);
	}

	return rc;
}

int main(int argc, char *argv[]) {
	signal(SIGINT, intHandler);
	
	PORTNUM = atoi(argv[1]);
	
	fd = open(argv[2], O_RDWR);
    assert(fd > -1);
    struct stat sbuf;
    int rc = fstat(fd, &sbuf);
    assert(rc > -1);

    int image_size = (int) sbuf.st_size;

    head = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	SUPERBLOCKPTR = (super_t*) head;
    assert(image != MAP_FAILED);
    sd = UDP_Open(PORTNUM);
    assert(sd > -1);

	in_bm_addr = head + SUPERBLOCKPTR->inode_bitmap_addr * UFS_BLOCK_SIZE;
    d_bm_addr = head + SUPERBLOCKPTR->data_bitmap_addr * UFS_BLOCK_SIZE;
    in_rn_addr = head + SUPERBLOCKPTR->inode_region_addr * UFS_BLOCK_SIZE;
    d_rn_addr = head + SUPERBLOCKPTR->data_region_addr * UFS_BLOCK_SIZE;

    while (1) {
		message_t m;
		
		int rc = UDP_Read(sd, &client_addr, (char *) &m, sizeof(message_t));
		if (rc > 0) 
		{
			rc = message_parser(&m);
		} 
	}
	return 0; 
}