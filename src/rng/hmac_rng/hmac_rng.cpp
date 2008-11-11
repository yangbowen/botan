/*************************************************
* HMAC_RNG Source File                           *
* (C) 2008 Jack Lloyd                            *
*************************************************/

#include <botan/hmac_rng.h>
#include <botan/entropy.h>
#include <botan/loadstor.h>
#include <botan/xor_buf.h>
#include <botan/util.h>
#include <botan/bit_ops.h>
#include <botan/stl_util.h>
#include <algorithm>

namespace Botan {

namespace {

void hmac_prf(MessageAuthenticationCode* prf,
            MemoryRegion<byte>& K,
            u32bit& counter,
            const std::string& label)
   {
   prf->update(K, K.size());
   prf->update(label);
   for(u32bit i = 0; i != 4; ++i)
      prf->update(get_byte(i, counter));
   prf->final(K);

   ++counter;
   }

}

/*************************************************
* Generate a buffer of random bytes              *
*************************************************/
void HMAC_RNG::randomize(byte out[], u32bit length)
   {
   /* Attempt to seed if we are currently not seeded, or if the
      counter is greater than 2^20

      If HMAC_RNG is wrapped in an X9.31/AES PRNG (the default), this
      means a reseed will be kicked off every 16 MiB of RNG output.
   */
   if(!is_seeded() || counter >= 0x100000)
      {
      reseed();

      if(!is_seeded())
         throw PRNG_Unseeded(name() + " seeding attempt failed");
      }

   /*
    HMAC KDF as described in E-t-E, using a CTXinfo of "rng"
   */
   while(length)
      {
      hmac_prf(prf, K, counter, "rng");

      const u32bit copied = std::min(K.size(), length);

      copy_mem(out, K.begin(), copied);
      out += copied;
      length -= copied;
      }

   /* Every once in a while do a fast poll of a entropy source */
   if(entropy_sources.size() && (counter % 65536 == 0))
      {
      u32bit got = entropy_sources.at(source_index)->
                       fast_poll(io_buffer, io_buffer.size());

      source_index = (source_index + 1) % entropy_sources.size();
      extractor->update(io_buffer, got);
      }
   }

/**
* Reseed the internal state, also accepting user input to include
*/
void HMAC_RNG::reseed_with_input(const byte input[], u32bit input_length)
   {
   if(entropy_sources.size())
      {
      /**
      Using the terminology of E-t-E, XTR is the MAC function (normally
      HMAC) seeded with XTS (below) and we form SKM, the key material, by
      fast polling each source, and then slow polling as many as we think
      we need (in the following loop), and feeding all of the poll
      results, along with any optional user input, along with, finally,
      feedback of the current PRK value, into the extractor function.
      */

      /*
      Previously this function did entropy estimation. However the paper

      "Boaz Barak, Shai Halevi: A model and architecture for
       pseudo-random generation with applications to /dev/random. ACM
       Conference on Computer and Communications Security 2005."

      provides a pretty strong case to not even try, since what we are
      really interested in is the *conditional* entropy from the point
      of view of an unknown attacker, which is impossible to
      calculate. They recommend, if an entropy estimate of some kind
      is needed, to use a low static estimate instead. We use here an
      estimate of 1 bit per byte.

      One thing I had been concerned about initially was that people
      without any randomness source enabled (much more likely in the
      days when you had to enable them manually) would find the RNG
      was unseeded and then pull the manuever some OpenSSL users did
      and seed the RNG with a constant string. However, upon further
      thought, I've decided that people who do that deserve to lose
      anyway.
      */

      for(u32bit j = 0; j < entropy_sources.size(); ++j)
         {
         const u32bit got =
            entropy_sources[j]->fast_poll(io_buffer, io_buffer.size());

         entropy += got;
         extractor->update(io_buffer, got);
         }

      for(u32bit j = 0; j != entropy_sources.size(); ++j)
         {
         const u32bit got =
            entropy_sources[j]->slow_poll(io_buffer, io_buffer.size());

         entropy += got;
         extractor->update(io_buffer, got);
         }
      }

   /*
   And now add the user-provided input, if any
   */
   if(input_length)
      {
      extractor->update(input, input_length);
      entropy += input_length;
      }

   /*
   It is necessary to feed forward poll data. Otherwise, a good
   poll (collecting a large amount of conditional entropy) followed
   by a bad one (collecting little) would be unsafe. Do this by
   generating new PRF outputs using the previous key and feeding them
   into the extractor function.

   Cycle the RNG once (CTXinfo="rng"), then generate a new PRF output
   using the CTXinfo "reseed". Provide these values as input to the
   extractor function.
   */
   hmac_prf(prf, K, counter, "rng");
   extractor->update(K); // K is the CTXinfo=rng PRF output

   hmac_prf(prf, K, counter, "reseed");
   extractor->update(K); // K is the CTXinfo=reseed PRF output

   /* Now derive the new PRK using everything that has been fed into
      the extractor, and set the PRF key to that */
   prf->set_key(extractor->final());

   // Now generate a new PRF output to use as the XTS extractor salt
   hmac_prf(prf, K, counter, "xts");
   extractor->set_key(K, K.size());

   // Reset state
   K.clear();
   counter = 0;

   // Upper bound entropy estimate at the extractor output size
   entropy = std::min<u32bit>(entropy, 8 * extractor->OUTPUT_LENGTH);
   }

/**
* Reseed the internal state
*/
void HMAC_RNG::reseed()
   {
   reseed_with_input(0, 0);
   }

/**
Add user-supplied entropy by reseeding and including this
input among the poll data
*/
void HMAC_RNG::add_entropy(const byte input[], u32bit length)
   {
   reseed_with_input(input, length);
   }

/*************************************************
* Add another entropy source to the list         *
*************************************************/
void HMAC_RNG::add_entropy_source(EntropySource* src)
   {
   entropy_sources.push_back(src);
   }

/*************************************************
* Check if the the pool is seeded                *
*************************************************/
bool HMAC_RNG::is_seeded() const
   {
   return (entropy >= 8 * prf->OUTPUT_LENGTH);
   }

 /*************************************************
* Clear memory of sensitive data                 *
*************************************************/
void HMAC_RNG::clear() throw()
   {
   extractor->clear();
   prf->clear();
   K.clear();
   entropy = 0;
   counter = 0;
   source_index = 0;
   }

/*************************************************
* Return the name of this type                   *
*************************************************/
std::string HMAC_RNG::name() const
   {
   return "HMAC_RNG(" + extractor->name() + "," + prf->name() + ")";
   }

/*************************************************
* HMAC_RNG Constructor                           *
*************************************************/
HMAC_RNG::HMAC_RNG(MessageAuthenticationCode* extractor_mac,
                   MessageAuthenticationCode* prf_mac) :
   extractor(extractor_mac), prf(prf_mac), io_buffer(128)
   {
   entropy = 0;

   // First PRF inputs are all zero, as specified in section 2
   K.create(prf->OUTPUT_LENGTH);
   counter = 0;
   source_index = 0;

   /*
   Normally we want to feedback PRF output into the input to the
   extractor function to ensure a single bad poll does not damage the
   RNG, but obviously that is meaningless to do on the first poll.

   We will want to use the PRF before we set the first key (in
   reseed_with_input), and it is a pain to keep track if it is set or
   not. Since the first time it doesn't matter anyway, just set it to
   a constant: randomize() will not produce output unless is_seeded()
   returns true, and that will only be the case if the estimated
   entropy counter is high enough. That variable is only set when a
   reseeding is performed.
   */
   std::string prf_key = "Botan HMAC_RNG PRF";
   prf->set_key(reinterpret_cast<const byte*>(prf_key.c_str()),
                prf_key.length());

   /*
   This will be used as the first XTS value when extracting input.
   XTS values after this one are generated using the PRF.

   If I understand the E-t-E paper correctly (specifically Section 4),
   using this fixed extractor key is safe to do.
   */
   std::string xts = "Botan HMAC_RNG XTS";
   extractor->set_key(reinterpret_cast<const byte*>(xts.c_str()),
                      xts.length());
   }

/*************************************************
* HMAC_RNG Destructor                            *
*************************************************/
HMAC_RNG::~HMAC_RNG()
   {
   delete extractor;
   delete prf;

   std::for_each(entropy_sources.begin(), entropy_sources.end(),
                 del_fun<EntropySource>());

   entropy = 0;
   counter = 0;
   }

}
