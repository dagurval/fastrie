#include"global.h"
#include <chrono>
#include "updater.h"

#define zeroesBeforeHashInPrime	8
#define ARCH_SSE4 0
#define ARCH_AVX 1
#define ARCH_AVX2 2

#define ARCH 0
#define DEBUG 0

/* AMD on susitna seems to like:  *2 sieveSize, 14.9m prime test limit */

static const int core_prime_n = 8; /* 8=23, 9=29 */
uint32 riecoin_sieveSize = 1024*1024*8; /* 1MB, tuned for L3 of Haswell */
/* 131 works here on haswell */
uint32_t riecoin_primeTestLimit;
#define KILL_UP_TO 6
static const int kill_up_to = KILL_UP_TO;
static const unsigned int vector_batch4_limit = 1000000;

uint32 riecoin_primorialSizeSkip = 40; /* 15 is the 64 bit limit */
static const uint32_t startPrime = riecoin_primorialSizeSkip;

//static const uint32_t primorial_offset = 97;
static const uint32_t primorial_offset = 16057; /* For > 26 or so */

#if ARCH==ARCH_SSE4
#define USE_VECTOR 0
#elif ARCH==ARCH_AVX
#define USE_VECTOR 1
#elif ARCH==ARCH_AVX2
#define USE_VECTOR 1
#endif

unsigned int int_invert_mpz(mpz_t &z_a, uint32_t nPrime);

const uint32 riecoin_denseLimit = 16384; /* A few cachelines */
uint32* riecoin_primeTestTable;
uint32 riecoin_primeTestSize;
int32_t *inverts;

mpz_t  z_primorial;
uint32_t primes_for_six = 0;  /* # test primes < denseLimit  */

void riecoin_init(uint64_t sieveMax)
{
        riecoin_primeTestLimit = sieveMax;
	printf("Generating table of small primes for Riecoin...\n");
	// generate prime table
	riecoin_primeTestTable = (uint32*)malloc(sizeof(uint32)*(riecoin_primeTestLimit/4+10));
	riecoin_primeTestSize = 0;
	  //int32_t inverted = int_invert_mpz(z_temp2, p);

	// generate prime table using Sieve of Eratosthenes
	uint8* vfComposite = (uint8*)malloc(sizeof(uint8)*(riecoin_primeTestLimit+7)/8);
	memset(vfComposite, 0x00, sizeof(uint8)*(riecoin_primeTestLimit+7)/8);
	for (unsigned int nFactor = 2; nFactor * nFactor < riecoin_primeTestLimit; nFactor++)
	{
		if( vfComposite[nFactor>>3] & (1<<(nFactor&7)) )
			continue;
		for (unsigned int nComposite = nFactor * nFactor; nComposite < riecoin_primeTestLimit; nComposite += nFactor)
			vfComposite[nComposite>>3] |= 1<<(nComposite&7);
	}
	for (unsigned int n = 2; n < riecoin_primeTestLimit; n++)
	{
		if ( (vfComposite[n>>3] & (1<<(n&7)))==0 )
		{
			riecoin_primeTestTable[riecoin_primeTestSize] = n;
			riecoin_primeTestSize++;
		}
	}
	riecoin_primeTestTable = (uint32*)realloc(riecoin_primeTestTable, sizeof(uint32)*riecoin_primeTestSize);
	free(vfComposite);
#if DEBUG
	printf("Table with %d entries generated\n", riecoin_primeTestSize);
#endif
	// make sure sieve size is divisible by 8
	riecoin_sieveSize = (riecoin_sieveSize&~7);
	// generate primorial for 40
	mpz_init_set_ui(z_primorial, riecoin_primeTestTable[0]);
	for(uint32 i=1; i<riecoin_primorialSizeSkip; i++)
	{
		mpz_mul_ui(z_primorial, z_primorial, riecoin_primeTestTable[i]);
	}
#if DEBUG
	gmp_printf("z_primorial: %Zd\n", z_primorial);
#endif
	inverts = (int32_t *)malloc(sizeof(int32_t) * (riecoin_primeTestSize));
	for (uint32_t i = 5; i < riecoin_primeTestSize; i++) {
	  inverts[i] = int_invert_mpz(z_primorial, riecoin_primeTestTable[i]);
	}

}

typedef uint32_t sixoff[6];

thread_local uint8* riecoin_sieve = NULL;
thread_local sixoff *offsets = NULL;


uint32 _getHexDigitValue(uint8 c)
{
	if( c >= '0' && c <= '9' )
		return c-'0';
	else if( c >= 'a' && c <= 'f' )
		return c-'a'+10;
	else if( c >= 'A' && c <= 'F' )
		return c-'A'+10;
	return 0;
}

/*
 * Parses a hex string
 * Length should be a multiple of 2
 */
void debug_parseHexString(char* hexString, uint32 length, uint8* output)
{
	uint32 lengthBytes = length / 2;
	for(uint32 i=0; i<lengthBytes; i++)
	{
		// high digit
		uint32 d1 = _getHexDigitValue(hexString[i*2+0]);
		// low digit
		uint32 d2 = _getHexDigitValue(hexString[i*2+1]);
		// build byte
		output[i] = (uint8)((d1<<4)|(d2));	
	}
}

void debug_parseHexStringLE(char* hexString, uint32 length, uint8* output)
{
	uint32 lengthBytes = length / 2;
	for(uint32 i=0; i<lengthBytes; i++)
	{
		// high digit
		uint32 d1 = _getHexDigitValue(hexString[i*2+0]);
		// low digit
		uint32 d2 = _getHexDigitValue(hexString[i*2+1]);
		// build byte
		output[lengthBytes-i-1] = (uint8)((d1<<4)|(d2));	
	}
}

unsigned int int_invert_internal(int32_t rem1, uint32_t nPrime)
{
  // Extended Euclidean algorithm to calculate the inverse of a in finite field defined by nPrime
  int32_t rem0 = nPrime;
  int32_t rem2;
  int32_t  aux0 = 0, aux1 = 1, aux2;
  int32_t quotient;
  int32_t inverse;

	while (1)
	{
		if (rem1 <= 1)
		{
			inverse = aux1;
			break;
		}

		rem2 = rem0 % rem1;
		quotient = rem0 / rem1;
		aux2 = -quotient * aux1 + aux0;

		if (rem2 <= 1)
		{
			inverse = aux2;
			break;
		}

		rem0 = rem1 % rem2;
		quotient = rem1 / rem2;
		aux0 = -quotient * aux2 + aux1;

		if (rem0 <= 1)
		{
			inverse = aux0;
			break;
		}

		rem1 = rem2 % rem0;
		quotient = rem2 / rem0;
		aux1 = -quotient * aux0 + aux2;
	}

	return (inverse + nPrime) % nPrime;
}

unsigned int int_invert_mpz(mpz_t &z_a, uint32_t nPrime)
{
  int32_t rem1 = mpz_tdiv_ui(z_a, nPrime); // rem1 = a % nPrime
  return int_invert_internal(rem1, nPrime);
  //  return int_invert_internal2(rem1, nPrime);
}

inline void silly_sort_indexes(uint32_t indexes[6]) {
  for (int i = 0; i < 5; i++) {
    for (int j = i+1; j < 6; j++) {
      if (indexes[j] < indexes[i]) {
	std::swap(indexes[i], indexes[j]);
      }
    }
  }
}

inline void silly_sort_indexes4(uint32_t indexes[KILL_UP_TO]) {
  for (int i = 0; i < kill_up_to-1; i++) {
    for (int j = i+1; j < kill_up_to; j++) {
      if (indexes[j] < indexes[i]) {
	std::swap(indexes[i], indexes[j]);
      }
    }
  }
}

inline void add_to_pending(uint8_t *sieve, uint32_t pending[16], uint32_t &pos, uint32_t ent) {
  __builtin_prefetch(&(sieve[ent>>3]));
  uint32_t old = pending[pos];
  if (old != 0) {
    sieve[old>>3] |= (1<<(old&7));
  }
  pending[pos] = ent;
  pos++;
  pos &= 0xf;
}


void riecoin_process(minerRiecoinBlock_t* block)
{
	uint32 searchBits = block->targetCompact;

	if( riecoin_sieve ) {
	  //memset(riecoin_sieve, 0x00, riecoin_sieveSize/8);
	}
	else
	{
		riecoin_sieve = (uint8*)malloc(riecoin_sieveSize/8);
		size_t offsize = sizeof(sixoff) * (riecoin_primeTestSize+1);
		offsets = (sixoff *)malloc(offsize);
		memset(offsets, 0, offsize);
	}
	uint8* sieve = riecoin_sieve;

#if DEBUG
	auto start = std::chrono::system_clock::now();
#endif

	// test data
	// getblock 16ee31c116b75d0299dc03cab2b6cbcb885aa29adf292b2697625bc9d28b2b64
	//debug_parseHexStringLE("c59ba5357285de73b878fed43039a37f85887c8960e66bcb6e86bdad565924bd", 64, block->merkleRoot);
	//block->version = 2;
	//debug_parseHexStringLE("c64673c670fb327c2e009b3b626d2def01d51ad4131a7a1040e9cef7bfa34838", 64, block->prevBlockHash);
	//block->nTime = 1392151955;
	//block->nBits = 0x02013000;
	//debug_parseHexStringLE("0000000000000000000000000000000000000000000000000000000070b67515", 64, block->nOffset);
	// generate PoW hash (version to nBits)
	uint8 powHash[32];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, (uint8*)block, 80);
	sha256_final(&ctx, powHash);
	sha256_init(&ctx);
	sha256_update(&ctx, powHash, 32);
	sha256_final(&ctx, powHash);
	// generatePrimeBase
	uint32* powHashU32 = (uint32*)powHash;
	mpz_t z_target;
	mpz_t z_temp;
	mpz_init(z_temp);
	mpz_t z_temp2;
	mpz_init(z_temp2);
	mpz_t z_remainderPrimorial;
	mpz_init(z_remainderPrimorial);

	mpz_init_set_ui(z_target, 1);
	mpz_mul_2exp(z_target, z_target, zeroesBeforeHashInPrime);
	for(uint32 i=0; i<256; i++)
	{
		mpz_mul_2exp(z_target, z_target, 1);
		if( (powHashU32[i/32]>>(i))&1 )
			z_target->_mp_d[0]++;
	}
	unsigned int trailingZeros = searchBits - 1 - zeroesBeforeHashInPrime - 256;
	mpz_mul_2exp(z_target, z_target, trailingZeros);
	// find first offset where target%primorial = primorial_offset

	mpz_tdiv_r(z_remainderPrimorial, z_target, z_primorial);
	mpz_abs(z_remainderPrimorial, z_remainderPrimorial);
	mpz_sub(z_remainderPrimorial, z_primorial, z_remainderPrimorial);
	mpz_tdiv_r(z_remainderPrimorial, z_remainderPrimorial, z_primorial);
	mpz_abs(z_remainderPrimorial, z_remainderPrimorial);
	mpz_add_ui(z_remainderPrimorial, z_remainderPrimorial, primorial_offset);

	mpz_add(z_temp, z_target, z_remainderPrimorial);

	mpz_t z_ft_r;
	mpz_init(z_ft_r);
	mpz_t z_ft_b;
	mpz_init_set_ui(z_ft_b, 2);
	mpz_t z_ft_n;
	mpz_init(z_ft_n);

	static uint32 primeTupleBias[6] = {0,4,6,10,12,16};
	mpz_set(z_temp2, z_primorial);

	uint32_t primeIndex = riecoin_primorialSizeSkip;
	
	uint32_t off_offset = 0;
	uint32_t startingPrimeIndex = primeIndex;
	uint32_t n_dense = 0;
	uint32_t n_sparse = 0;
	mpz_set(z_temp2, z_primorial);
	//printf("primeIndex: %d  is %d\n", primeIndex, riecoin_primeTestTable[primeIndex]);
	for( ; primeIndex < riecoin_primeTestSize; primeIndex++)
	{
	  uint32 p = riecoin_primeTestTable[primeIndex];
	  int32_t inverted = inverts[primeIndex];
	  if (p < riecoin_denseLimit) {
	    n_dense++;
	  } else {
	    n_sparse++;
	  }

	  uint32 remainder = mpz_tdiv_ui(z_temp, p);
	  for (uint32 f = 0; f < 6; f++) {
	    uint64_t b_remainder = remainder + primeTupleBias[f];
	    b_remainder %= p;
	    int64_t pa = (p<b_remainder)?(p-b_remainder+p):(p-b_remainder);
	    uint64_t index = (pa%p)*inverted;
	    index %= p;
	    offsets[off_offset][f] = index;
	  }
	  silly_sort_indexes(offsets[off_offset]);
	  off_offset++;
	}

#if DEBUG
	auto end = std::chrono::system_clock::now();
	auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();
	printf("Initial invert time:  %d ms\n", dur);
#endif

	/* Main processing loop:
	 * 1)  Sieve "dense" primes;
	 * 2)  Sieve "sparse" primes;
	 * 3)  Scan sieve for candidates, test, report
	 */

	uint32 countCandidates = 0;
	uint32 countPrimes = 0;

	static const int maxiter = 100; /* XXX */
	for (int loop = 0; loop < maxiter; loop++) {
	    __sync_synchronize(); /* gcc specific - memory barrier for checking height */
	    if( block->height != monitorCurrentBlockHeight ) {
	      break;
	    }

	    memset(sieve, 0, riecoin_sieveSize/8);

	    for (unsigned int i = 0; i < n_dense; i++) {
	      silly_sort_indexes(offsets[i]);
	      uint32_t p = riecoin_primeTestTable[i+startingPrimeIndex];
	      for (uint32 f = 0; f < 6; f++) {
		while (offsets[i][f] < riecoin_sieveSize) {
		  sieve[offsets[i][f]>>3] |= (1<<((offsets[i][f]&7)));
		  offsets[i][f] += p;
		}
		offsets[i][f] -= riecoin_sieveSize;
	      }
	    }
	    

	    uint32_t pending[16];
	    uint32_t pending_pos = 0;
	    for (int i = 0; i < 16; i++) { pending[i] = 0; }
	    
	    for (unsigned int i = n_dense; i < (n_dense+n_sparse); i++) {
	      uint32_t p = riecoin_primeTestTable[i+startingPrimeIndex];
	      if (i < (n_sparse - 16)) {
		__builtin_prefetch(&offsets[i+16][0]);
	      }
	      for (uint32 f = 0; f < 6; f++) {
		while (offsets[i][f] < riecoin_sieveSize) {
		  add_to_pending(sieve, pending, pending_pos, offsets[i][f]);
		  offsets[i][f] += p;
		}
		offsets[i][f] -= riecoin_sieveSize;
	      }
	    }

	    for (int i = 0; i < 16; i++) {
	      uint32_t old = pending[i];
	      sieve[old>>3] |= (1<<(old&7));
	    }
	  

	    // scan for candidates
	    for(uint32 i=1; i<riecoin_sieveSize; i++) {
	      if( sieve[(i)>>3] & (1<<((i)&7)) )
		continue;
	      countCandidates++;

	      /* Check for a prime cluster.  A "share" on ypool is any
	       * four or more of the elements prime, but for speed,
	       * check further only if the first passes the primality
	       * test.  The first test is the bottleneck for the
	       * miner.
	       *
	       * Uses the fermat test - jh's code noted that it is slightly faster.
	       * Could do an MR test as a follow-up, but the server can do this too
	       * for the one-in-a-whatever case that Fermat is wrong.
	       */

	      int nPrimes = 0;
	      // p1

	      mpz_set(z_temp, z_primorial);
	      mpz_mul_ui(z_temp, z_temp, loop);
	      mpz_mul_ui(z_temp, z_temp, riecoin_sieveSize);
	      mpz_set(z_temp2, z_primorial);
	      mpz_mul_ui(z_temp2, z_temp2, i);
	      mpz_add(z_temp, z_temp, z_temp2);
	      mpz_add(z_temp, z_temp, z_remainderPrimorial);
	      mpz_add(z_temp, z_temp, z_target);

	      mpz_sub_ui(z_ft_n, z_temp, 1);
	      mpz_powm(z_ft_r, z_ft_b, z_ft_n, z_temp);
	      if (mpz_cmp_ui(z_ft_r, 1) != 0)
		continue;
	      else
		countPrimes++;

	      nPrimes++;

	      /* Low overhead but still often enough */
	      __sync_synchronize(); /* gcc specific - memory barrier for checking height */
	      if( block->height != monitorCurrentBlockHeight ) {
		break;
	      }

	      /* New definition of shares:  Any 4+ valid primes.  Search method 
	       * is for 1st + any 3 to avoid doing too much primality testing.
	       */

	      /* Note start at 1 - we've already tested bias 0 */
	      int prev_offset = 0;
	      for (int i = 1; i < 6; i++) {
		uint32_t add_to_offset = primeTupleBias[i] - prev_offset;
		prev_offset = primeTupleBias[i];
		mpz_add_ui(z_temp, z_temp, add_to_offset);
		mpz_sub_ui(z_ft_n, z_temp, 1);
		mpz_powm(z_ft_r, z_ft_b, z_ft_n, z_temp);
		if (mpz_cmp_ui(z_ft_r, 1) == 0) {
		  nPrimes++;
		}
                int candidatesRemaining = 5-i;
                if ((nPrimes + candidatesRemaining) < 4) { continue; }
	      }

	      /* These statistics are a little confusing because of the interaction
	       * with early-exit above.  They overcount relative to finding consecutive
	       * primes, but undercount relative to counting all primes.  But they're
	       * still useful for benchmarking within a variant of the program with
	       * all else held equal. */
	      if (nPrimes >= 2) total2ChainCount++;
	      if (nPrimes >= 3) total3ChainCount++;
	      if (nPrimes >= 4) total4ChainCount++;

	      if (nPrimes < 4) continue;

	      mpz_set(z_temp, z_primorial);
	      mpz_mul_ui(z_temp, z_temp, loop);
	      mpz_mul_ui(z_temp, z_temp, riecoin_sieveSize);
	      mpz_set(z_temp2, z_primorial);
	      mpz_mul_ui(z_temp2, z_temp2, i);
	      mpz_add(z_temp, z_temp, z_temp2);
	      mpz_add(z_temp, z_temp, z_remainderPrimorial);
	      mpz_add(z_temp, z_temp, z_target);

	      //	      mpz_add_ui(z_temp, z_target, (uint64)remainderPrimorial + (uint64)primorial*(uint64)i + (loop*riecoin_sieveSize*(uint64)primorial));

	      mpz_sub(z_temp2, z_temp, z_target);
	      // submit share
	      uint8 nOffset[32];
	      memset(nOffset, 0x00, 32);
#if defined _WIN64 || __X86_64__
	      for(uint32 d=0; d<std::min(32/8, z_temp2->_mp_size); d++)
		{
		  *(uint64*)(nOffset+d*8) = z_temp2->_mp_d[d];
		}
#elif defined _WIN32 
	      for(uint32 d=0; d<std::min(32/4, z_temp2->_mp_size); d++)
		{
		  *(uint32*)(nOffset+d*4) = z_temp2->_mp_d[d];
		}
#elif defined __GNUC__
#ifdef	__x86_64__
	      for(uint32 d=0; d<std::min(32/8, z_temp2->_mp_size); d++)
		{
		  *(uint64*)(nOffset+d*8) = z_temp2->_mp_d[d];
		}
#else  
	      for(uint32 d=0; d<std::min(32/4, z_temp2->_mp_size); d++)
		{
		  *(uint32*)(nOffset+d*4) = z_temp2->_mp_d[d];
		}
#endif
#endif
	      totalShareCount++;
	      xptMiner_submitShare(block, nOffset);
	    }
	}
	mpz_clears(z_target, z_temp, z_temp2, z_ft_r, z_ft_b, z_ft_n, z_remainderPrimorial, NULL);

}
