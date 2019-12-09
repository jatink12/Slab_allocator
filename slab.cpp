#include <iostream>
#include <unistd.h>
#include <unordered_map>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
#include <cassert>
#define GET_PAGESIZE() sysconf(_SC_PAGESIZE)

using namespace std;

enum SlabType { SMALL, LARGE};
struct mem_slab;

struct mem_bufctl {
    struct mem_bufctl * next_bufctl; //also freelist linkage
    struct mem_bufctl * prev_bufctl; //used in mem_cahce_destroy
    void * buff;
    struct mem_slab * parent_slab;

};


struct mem_slab {
    struct mem_slab * next_slab, *prev_slab;
    int refcount;
    struct mem_bufctl * free_buffctls;
    void * mem;
    unsigned int align;
    unsigned int color;
    void * bitvec;
    // int max_relevant_bit = cache->objs_per_slab;

};

struct mem_cache {
    char * name;
    size_t objsize;
    unsigned int align;

    unsigned int objs_per_slab;
    void (*constructor)(void *, size_t);
    void (*destructor)(void *, size_t);


    struct mem_slab * free_slabs; //first non-empty slab LL
    struct mem_slab * slabs; //doubly linked list of slabs /(not circ)
    struct mem_slab * lastslab;

    unsigned int lastcolor;

    unordered_map< void*, struct mem_bufctl *> btobctl;
    unordered_map< void *, pair<struct mem_slab *, unsigned int> > btoslab;
    //create hash table
    //for small objects this hash gives the slab address

    SlabType slabtype;



};


struct mem_slab * mem_allocate_small_slab ( unsigned int objsize,
        unsigned int align,
        unsigned int color,
        unsigned int objs_per_slab,
        void (*constructor) (void *,size_t),
        struct mem_cache * cache) {

    //create a slab object (using malloc, later use mem_cache), initialize, set free_buffctls to null
    struct mem_slab * slab = new mem_slab();
    slab->next_slab= NULL;
    slab->prev_slab = NULL;
    slab->refcount = 0;
    slab->free_buffctls = NULL;
    slab-> align = align;
    slab->color = color;
    unsigned int pagesize = GET_PAGESIZE();


    //mmap objsize bytes, aligned 0 (defaults to “get a page”) store in mem ptr
    slab->mem = mmap(NULL, objsize , PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    //jump to mem + pagesize – sizeof(mem_slab)  (maybe create an inline func to get this)
    void * slabptr = slab->mem + pagesize + slab->color - sizeof(mem_slab);
    memcpy(slabptr, slab, sizeof(struct mem_slab) );


    //cache->objs_per_slab = (int) floor  [ ( pagesize – sizeof(mem_slab) – color ) / objsize ]
    cache->objs_per_slab =  (pagesize - sizeof(mem_slab) - color )/ objsize ;

    //skip color bytes and construct cache->objs_per_slab objects.
    //Keep adding  <buffptr, pair<slab_ptr, buffindex >> to cache.btobctl (hashtable)

    //create dummy object
    void * dummy;
    constructor(dummy, objsize);

    void * tmp = slab->mem + color;
    for ( unsigned int i = 0 ; i < cache->objs_per_slab; i++) {
        memcpy (tmp, dummy, objsize);
        tmp += objsize;

        cache->btoslab[tmp] = make_pair ( (struct mem_slab *)slabptr, i) ;
    }


    //initialize bitvec
    unsigned int bytes_required = (unsigned int) ceil ( cache->objs_per_slab / 8.0f );
    slab->bitvec = malloc (bytes_required);
    memset(slab->bitvec, 0, bytes_required);


    return slab;




}

struct mem_slab* allocate_large_slab(
    size_t objsize,
    unsigned int obj_per_slab,
    struct mem_cache* cache,
    unsigned int color,
    unsigned int align,
    void (*constructor)(void *,size_t)
)
{
    struct mem_slab *newSlab=(struct mem_slab *)malloc(sizeof(mem_slab));
    newSlab->refcount=0;

    //create list of buffctls as DLL

    struct mem_bufctl *buffListHead=(struct mem_bufctl*)malloc(sizeof(mem_bufctl));
    buffListHead->next_bufctl=NULL;
    buffListHead->prev_bufctl=NULL;
    buffListHead->parent_slab=newSlab;

    //nothing but insert at end of DLL everytime
    struct mem_bufctl* temp=buffListHead;

    for(int i=0;i<obj_per_slab-1;i++)
    {
        struct mem_bufctl *newBuff=(struct mem_bufctl*)malloc(sizeof(mem_bufctl));
        temp->next_bufctl=newBuff;
        newBuff->prev_bufctl=temp;
        newBuff->next_bufctl=NULL;
        temp->parent_slab=newSlab;
        temp=newBuff;
    }

    newSlab->free_buffctls=buffListHead;

    //memory

    int SIZE = color + obj_per_slab * objsize;
    newSlab->mem=mmap(NULL,SIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);

    //skip color bytes and start constructing objects from newSlab->mem+color

    void *start=newSlab->mem+color;

    temp = newSlab->free_buffctls; //DOUBT


    void *dummy;
    constructor(dummy,objsize);

    for(int i=1;i<=obj_per_slab;i++)
    {
        memcpy(start,dummy,objsize);
        temp->buff=start;
        //buffer address is start
        //its bufctl address is temp
        cache->btobctl[start]=temp;

        temp=temp->next_bufctl;
        start=start+objsize;
    }

    cache->slabtype=LARGE;

    //slabs
    //free_slabs
    //last_slab

    if(cache->slabs==NULL)
    {
        cache->slabs=newSlab;
        cache->free_slabs=newSlab;
        cache->lastslab=newSlab;
        newSlab->prev_slab=NULL;
    }
    else
    {
        cache->lastslab->next_slab=newSlab;
        newSlab->prev_slab=cache->lastslab;
        cache->lastslab=newSlab;
        cache->free_slabs=newSlab;

    }

    return newSlab;
}




struct mem_cache *mem_cache_create (
    char * name,
    size_t objsize,
    unsigned int objs_per_slab,
    unsigned int align,
    void (*constructor)(void *, size_t),
    void (*destructor)(void *, size_t)
) {


    //initialize cache object
    struct mem_cache * cache = new mem_cache ();
    cache->name = name;
    cache->objsize = objsize;
    cache->objs_per_slab = objs_per_slab;
    cache->align = align;
    cache->constructor = constructor;
    cache->destructor = destructor;
    cache->lastcolor = 0;

    //check which slab type to create
    unsigned int pagesize = GET_PAGESIZE();
    struct mem_slab * newslab;

    if (objsize > pagesize / 8) {
        cache->slabtype = LARGE;
        //If large, create large slab, update slabtype

        newslab = allocate_large_slab(objsize,objs_per_slab,cache,cache->lastcolor,0,constructor);

    } else {


        cache->slabtype = SMALL;
        //If small, create small slab

        //calculate color
        unsigned int color = 0;


        newslab =   mem_allocate_small_slab ( objsize, align, color, objs_per_slab, constructor, cache);



    }

    //initialize free_slabs and slabs, lastcolor
    cache->free_slabs = cache->slabs = cache->lastslab = newslab;


    //unsigned int lastcolor;
    //lastcolor = (lastcolor + 8) % 32;
    //cache->lastcolor = lastcolor;


    return cache;

}


void * mem_cache_alloc (struct mem_cache * cache) {

    if(cache->free_slabs==NULL)
    {
        cache->lastcolor=(cache->lastcolor+8)%32;

        struct mem_slab* newSlab;
        if (cache->slabtype == LARGE) {
            newSlab = allocate_large_slab(cache->objsize,cache->objs_per_slab,cache,cache->lastcolor,0,cache->constructor);
        }
        else if (cache->slabtype == SMALL) {
            newSlab = mem_allocate_small_slab ( cache->objsize, cache->align, cache->lastcolor, cache->objs_per_slab, cache->constructor, cache);
        }


        cache->free_slabs=newSlab;
        newSlab->prev_slab=cache->lastslab;
        cache->lastslab=newSlab;
    }
    if (cache->slabtype = LARGE) {
        struct mem_slab *insertSlab=cache->free_slabs;

        void * objAddr = insertSlab->free_buffctls->buff;

        insertSlab->refcount++;

        insertSlab->free_buffctls = insertSlab->free_buffctls->next_bufctl;

        if(insertSlab->refcount == cache->objs_per_slab)
        {
            cache->free_slabs=cache->free_slabs->next_slab;
        }

        return objAddr;
    }


    // handling for small slabs
    // get first free slab using bitvector
    // go to free_slabs
    // get free buff from first 0 bit in bitvec and set it to 1, don’t check more than
    // cp->objs_per_slab 0s

    struct mem_slab * freeslab = cache->free_slabs;
    void * bitvec = freeslab->bitvec;
    unsigned int bytes_required = (unsigned int) ceil ( cache->objs_per_slab / 8.0f );
    unsigned int index = -1;
    for (int i = 0; i < bytes_required; i++) {
        char * chars = (char *) bitvec;
        for (int j = 0; j <8;j++) {
            if ( ~(*chars) &  (1<< (8-j-1) )) {
                index = i*8 + j;
                break;
            }
        }

    }
    if (index >= cache->objs_per_slab) {
        index = -1;
    }


    // Do pointer manipulation to get the appropriate buff.
    // inc refcount
    void * buff = freeslab->mem + freeslab->color + index * cache->objsize;

    freeslab->refcount++;


    //if refcount == objs_per_slab update free_slabs
    if (freeslab->refcount == cache->objs_per_slab) {
        cache->free_slabs = cache->free_slabs->next_slab;

    }

    return buff;
}


void mem_cache_free ( struct mem_cache * cache, void * buff) {
    if (cache->slabtype == LARGE) {

    }


    /*
    • If small obj, get slab, buffindex from hash
        ◦ goto slab->bitvec[buffindex] = 0
    • update refcount
    */

    struct mem_slab * slab = cache->btoslab[buff].first ;
    unsigned int buffindex = cache->btoslab[buff].second;

    unsigned int byteno = buffindex / 8;
    unsigned int bitno = buffindex % 8;

    char * chars = (char * ) slab->bitvec;
    chars += byteno;

    char bit = 1 << (8-1-bitno);
    assert (~(*chars) & bit );

    *chars ^= bit;

    slab->refcount--;


    //When a slab ref count becomes 0, move it to the end of slab list in the cache



}

void mem_cache_destroy (struct mem_cache * cache) {
    //Iterate over slabs

    struct mem_slab * slab = cache->slabs;
    if (cache->slabtype == LARGE) {
    //for large slabs
        while (slab != NULL) {

            munmap (slab->mem, cache->objs_per_slab * cache->objsize + slab->color);

            struct mem_bufctl * tbuf = slab->free_buffctls;
            struct mem_bufctl * tbufprev = tbuf->prev_bufctl;

            //free right bufctls
            while (tbuf != NULL) {
                struct mem_bufctl * tmp = tbuf;
                tbuf = tbuf->next_bufctl;
                delete tmp;
            }

            //free left bufctls
            while (tbufprev != NULL) {
                struct mem_bufctl * tmp = tbufprev;
                tbufprev = tbufprev->prev_bufctl;
                delete tmp;
            }


            //move to next slab and delete current slab
            struct mem_slab * pslab = slab;
            slab = slab->next_slab;
            delete pslab;
        }
    }
    else if ( cache->slabtype == SMALL) {
    //for small slabs
        while (slab != NULL) {
            delete slab->bitvec;

            struct mem_slab * pslab = slab;
            slab = slab->next_slab;

            munmap (pslab->mem, cache->objsize);


        }

    }


    delete cache;

}


int main() {
    cout << GET_PAGESIZE() << endl;
    return 0;
}

