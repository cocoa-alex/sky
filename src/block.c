#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include "dbg.h"
#include "endian.h"
#include "bstring.h"
#include "mem.h"
#include "block.h"
#include "path_iterator.h"

//==============================================================================
//
// Functions
//
//==============================================================================

//======================================
// Lifecycle
//======================================

// Creates a block object.
//
// Returns a reference to a new block object if successful. Otherwise
// returns null.
sky_block *sky_block_create(sky_data_file *data_file)
{
    sky_block *block = calloc(sizeof(sky_block), 1);
    check_mem(block);

    block->data_file = data_file;
    
    return block;
    
error:
    sky_block_free(block);
    return NULL;
}

// Removes a block object from memory.
void sky_block_free(sky_block *block)
{
    if(block) {
        memset(block, 0, sizeof(*block));
        free(block);
    }
}


//======================================
// Serialization
//======================================

// Packs a block object into memory at a given pointer.
//
// block - The block object to pack.
// ptr   - The pointer to the current location.
// sz    - The number of bytes written.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_pack(sky_block *block, void *ptr, size_t *sz)
{
    void *start = ptr;

    // Validate.
    check(block != NULL, "Block required");
    check(ptr != NULL, "Pointer required");
    
    // Write object id range.
    sky_object_id_t min_object_id = htonll(block->min_object_id);
    sky_object_id_t max_object_id = htonll(block->max_object_id);
    memwrite(ptr, &min_object_id, sizeof(min_object_id), "min object id");
    memwrite(ptr, &max_object_id, sizeof(max_object_id), "max object id");

    // Write timestamp range.
    sky_timestamp_t min_timestamp = htonll(block->min_timestamp);
    sky_timestamp_t max_timestamp = htonll(block->max_timestamp);
    memwrite(ptr, &min_timestamp, sizeof(min_timestamp), "min timestamp");
    memwrite(ptr, &max_timestamp, sizeof(max_timestamp), "max timestamp");

    // Store number of bytes written.
    if(sz != NULL) {
        *sz = (ptr-start);
    }
    
    return 0;

error:
    return -1;
}

// Unpacks a block object from memory at the current pointer.
//
// block - The block object to unpack into.
// ptr  - The pointer to the current location.
// sz   - The number of bytes read.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_unpack(sky_block *block, void *ptr, size_t *sz)
{
    void *start = ptr;
    
    // Validate.
    check(block != NULL, "Block required");
    check(ptr != NULL, "Pointer required");

    // Write object id range.
    memread(ptr, &block->min_object_id, sizeof(block->min_object_id), "min object id");
    block->min_object_id = ntohll(block->min_object_id);
    memread(ptr, &block->max_object_id, sizeof(block->max_object_id), "max object id");
    block->max_object_id = ntohll(block->max_object_id);

    // Write timestamp range.
    memread(ptr, &block->min_timestamp, sizeof(block->min_timestamp), "min timestamp");
    block->min_timestamp = ntohll(block->min_timestamp);
    memread(ptr, &block->max_timestamp, sizeof(block->max_timestamp), "max timestamp");
    block->max_timestamp = ntohll(block->max_timestamp);

    // Store number of bytes read.
    if(sz != NULL) {
        *sz = (ptr-start);
    }
    
    return 0;

error:
    *sz = 0;
    return -1;
}


//======================================
// Block Position
//======================================

// Calculates the byte offset for the beginning on the block in the
// data file based on the data file block size and the block index.
//
// block  - The block to calculate the byte offset of.
// offset - A pointer to where the offset will be returned to.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_get_offset(sky_block *block, size_t *offset)
{
    check(block != NULL, "Block required");
    check(block->data_file != NULL, "Data file required");
    check(block->data_file->block_size != 0, "Data file must have a nonzero block size");

    *offset = (block->data_file->block_size * block->index);
    return 0;

error:
    *offset = 0;
    return -1;
}

// Calculates the pointer position for the beginning on the block in the
// data file based on the data file block size and the block index.
//
// block - The block to calculate the byte offset of.
// ptr   - A pointer to where the blocks starting address will be set.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_get_ptr(sky_block *block, void **ptr)
{
    check(block != NULL, "Block required");
    check(block->data_file != NULL, "Data file required");
    check(block->data_file->data != NULL, "Data file must be mapped");

    // Retrieve the offset.
    size_t offset;
    int rc = sky_block_get_offset(block, &offset);
    check(rc == 0, "Unable to determine block offset");

    // Calculate pointer based on data pointer.
    *ptr = block->data_file->data + offset;
    
    return 0;

error:
    *ptr = NULL;
    return -1;
}


//======================================
// Spanning
//======================================

// Determines the number of blocks that this block's object spans. This
// function only works on the starting block of a span of blocks.
//
// block - The initial block in a block span.
// count - A pointer to where the number of spanned blocks should be stored.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_get_span_count(sky_block *block, uint32_t *count)
{
    check(block != NULL, "Block required");
    check(block->data_file != NULL, "Data file required");
    check(count != NULL, "Span count address required");
    
    sky_data_file *data_file = block->data_file;
    sky_block **blocks = data_file->blocks;

    // If this block is not spanned then return 1.
    if(!block->spanned) {
        *count = 1;
    }
    // Otherwise calculate the span count.
    else {
        // Loop until the ending block of the span is found.
        uint32_t index = block->index;
        sky_object_id_t object_id = block->min_object_id;
        while(true) {
            index++;

            // If we've reached the end of the blocks or if the object id no longer
            // matches the starting object id then break out of the loop.
            if(index > data_file->block_count-1 || object_id != blocks[index]->min_object_id)
            {
                break;
            }
        }

        // Assign count back to caller's provided address.
        *count = (index - block->index);
    }
    
    return 0;

error:
    *count = 0;
    return -1;
}


//======================================
// Splitting
//======================================

// Splits a block into multiple blocks based on the the addition of an event.
// The paths are placed into multiple buckets depending on their sizes and
// order and then are evenly distributed across the blocks.
//
// block - The block to split.
// event - The event that will be added to the block.
// ret   - A reference to the block that the event will be added to.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_split_with_event(sky_block *block, sky_event *event,
                               sky_block **ret)
{
    int rc;
    check(block != NULL, "Block required");
    check(event != NULL, "Event required");
    
    // Initialize the return value.
    *ret = NULL;
    
    // Create a path iterator and point it at the block.
    sky_path_iterator *iterator = sky_path_iterator_create();
    rc = sky_path_iterator_set_block(iterator, block);
    check(rc == 0, "Unable to set path iterator block");
    
    void *ptr;
    sky_object_id_t object_id = event->object_id;

    // Loop over paths in block.
    while(!iterator->eof) {
        // If the current path matches then save the ptr and exit.
        if(object_id == iterator->current_object_id) {
            rc = sky_path_iterator_get_ptr(iterator, &ptr);
            check(rc == 0, "Unable to retrieve iterator's current pointer");
            break;
        }
    }
    
    sky_path_iterator_free(iterator);
    
    return 0;

error:
    return -1;
}


//======================================
// Path Management
//======================================

// Retrieves a pointer to the start of a path with a given object id inside
// the block. If no path exists with the given object id exists then a null
// pointer is returned.
//
// block     - The block to search.
// object_id - The object id to search for.
// ret       - A pointer to where the path's pointer should be returned to.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_get_path_ptr(sky_block *block, sky_object_id_t object_id,
                           void **ret)
{
    int rc;
    check(block != NULL, "Block required");
    check(object_id > 0, "Object id required");

    // Create a path iterator and point it at the block.
    sky_path_iterator *iterator = sky_path_iterator_create();
    rc = sky_path_iterator_set_block(iterator, block);
    check(rc == 0, "Unable to set path iterator block");
    
    // Initialize the return value.
    *ret = NULL;
    
    // Loop over iterator until we find the path matching the object id.
    while(!iterator->eof) {
        // If the current path matches then save the ptr and exit.
        if(object_id == iterator->current_object_id) {
            rc = sky_path_iterator_get_ptr(iterator, ret);
            check(rc == 0, "Unable to retrieve iterator's current pointer");
            break;
        }
    }
    
    sky_path_iterator_free(iterator);
    
    return 0;

error:
    sky_path_iterator_free(iterator);
    *ret = NULL;
    return -1;
}



//======================================
// Event Management
//======================================

// Adds an event to the block. If the size of the block exceeds the block size
// then a new empty block is allocated and half the paths in the block are
// moved to the new block.
//
// block - The block to add the event to.
// event - The event to add to the block.
//
// Returns 0 if successful, otherwise returns -1.
int sky_block_add_event(sky_block *block, sky_event *event)
{
    check(block != NULL, "Block required");
    check(event != NULL, "Event required");

    // If block will be too large then split the block.
    
    // TODO: If split, determine if event should be added to new block.
    // TODO: Find path inside block or where new path should be inserted.
    // TODO: Serialize path/event and insert into insertion point.
    
    return 0;

error:
    return -1;
}
