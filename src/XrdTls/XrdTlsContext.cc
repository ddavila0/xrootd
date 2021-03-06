//------------------------------------------------------------------------------
// Copyright (c) 2011-2018 by European Organization for Nuclear Research (CERN)
// Author: Michal Simon <simonm@cern.ch>
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#include <stdio.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/stat.h>

#include "XrdOuc/XrdOucUtils.hh"
#include "XrdSys/XrdSysAtomics.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysPthread.hh"

#include "XrdTls/XrdTls.hh"
#include "XrdTls/XrdTlsContext.hh"

#if __cplusplus >= 201103L
#include <atomic>
#endif

/******************************************************************************/
/*                               G l o b a l s                                */
/******************************************************************************/

namespace XrdTlsGlobal
{
extern XrdTls::msgCB_t msgCB;
};
  
/******************************************************************************/
/*                      X r d T l s C o n t e x t I m p l                     */
/******************************************************************************/

struct XrdTlsContextImpl
{
    XrdTlsContextImpl() : ctx( 0 ) { }
   ~XrdTlsContextImpl() {if (ctx) SSL_CTX_free(ctx);}

    SSL_CTX                      *ctx;
    XrdTlsContext::CTX_Params     Parm;
};
  
/******************************************************************************/
/*                 S S L   T h r e a d i n g   S u p p o r t                  */
/******************************************************************************/

// The following may confusing because SSL MT support is somewhat bizarre.
// Versions  < 1.0 require a numeric thread_id and lock callbasks.
// Versions  < 1.1 require a lock_callbacks but the thread_is callback is
//                 optional. While the numeric thread_id callback can be used
//                 it's deprecated and fancier pointer/numeric call should be
//                 used. In our case, we use the deprecated version.
// Versions >- 1.1 Do not need any callbacks as all threading functions are
//                 internally defined to use native MT functions.
  
#if OPENSSL_VERSION_NUMBER < 0x10100000L && defined(OPENSSL_THREADS)
namespace
{
#define XRDTLS_SET_CALLBACKS 1
#ifdef __solaris__
extern "C" {
#endif

unsigned long sslTLS_id_callback(void)
{
   return (unsigned long)XrdSysThread::ID();
}

XrdSysMutex *MutexVector = 0;

void sslTLS_lock(int mode, int n, const char *file, int line)
{
// Use exclusive locks. At some point, SSL categorizes these as read and
// write locks but it's not clear when this actually occurs, sigh.
//
   if (mode & CRYPTO_LOCK) MutexVector[n].Lock();
      else                 MutexVector[n].UnLock();
}
#ifdef __solaris__
}
#endif
}   // namespace
#else
#undef XRDTLS_SET_CALLBACKS
#endif

/******************************************************************************/
/*                F i l e   L o c a l   D e f i n i t i o n s                 */
/******************************************************************************/
  
namespace
{
// The following is the default cipher list. Note that for OpenSSL v1.0.2+ we
// use the recommended cipher list from Mozilla. Otherwise, we use the dumber
// less secure ciphers as older versions of openssl have issues with them. See
// ssl-config.mozilla.org/#config=intermediate&openssl=1.0.2k&guideline=5.4
//
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
const char *sslCiphers = "ECDHE-ECDSA-AES128-GCM-SHA256:"
                         "ECDHE-RSA-AES128-GCM-SHA256:"
                         "ECDHE-ECDSA-AES256-GCM-SHA384:"
                         "ECDHE-RSA-AES256-GCM-SHA384:"
                         "ECDHE-ECDSA-CHACHA20-POLY1305:"
                         "ECDHE-RSA-CHACHA20-POLY1305:"
                         "DHE-RSA-AES128-GCM-SHA256:"
                         "DHE-RSA-AES256-GCM-SHA384";
#else
const char *sslCiphers = "ALL:!LOW:!EXP:!MD5:!MD2";
#endif

XrdSysMutex            ctxMutex;
#if __cplusplus >= 201103L
std::atomic<bool>      initDone( false );
#else
bool                   initDone = false;
#endif

/******************************************************************************/
/*                               I n i t T L S                                */
/******************************************************************************/
  
void InitTLS() // This is strictly a one-time call!
{
   XrdSysMutexHelper ctxHelper(ctxMutex);

// Make sure we are not trying to load the ssl library more than once. This can
// happen when a server and a client instance happen to be both defined.
//
   if (initDone) return;
   initDone = true;

// SSL library initialisation
//
   SSL_library_init();
   OpenSSL_add_all_algorithms();
   SSL_load_error_strings();
   OpenSSL_add_all_ciphers();
   ERR_load_BIO_strings();
   ERR_load_crypto_strings();

// Set callbacks if we need to do this
//
#ifdef XRDTLS_SET_CALLBACKS

   int n =  CRYPTO_num_locks();
   if (n > 0)
      {MutexVector = new XrdSysMutex[n];
       CRYPTO_set_locking_callback(sslTLS_lock);
      }
   CRYPTO_set_id_callback(sslTLS_id_callback);

#endif
}

/******************************************************************************/
/*                          G e t T l s M e t h o d                           */
/******************************************************************************/

const char *GetTlsMethod(const SSL_METHOD *&meth)
{
#ifdef HAVE_TLS
  meth = TLS_method();
#else
  meth = SSLv23_method();
#endif
  if (meth == 0) return "No negotiable TLS method available.";
  return 0;
}
  
/******************************************************************************/
/*                              V e r P a t h s                               */
/******************************************************************************/

bool VerPaths(const char *cert, const char *pkey,
              const char *cadr, const char *cafl, std::string &eMsg)
{
   static const mode_t cert_mode = S_IRUSR | S_IWUSR | S_IRWXG | S_IROTH;
   static const mode_t pkey_mode = S_IRUSR | S_IWUSR;
   static const mode_t cadr_mode = S_IRWXU | S_IRGRP | S_IXGRP
                                           | S_IROTH | S_IXOTH;
   static const mode_t cafl_mode = S_IRUSR | S_IWUSR | S_IRWXG | S_IROTH;
   const char *emsg;

// If the ca cert directory is present make sure it's a directory and
// only the ower can write to that directory (anyone can read from it).
//
   if (cadr && (emsg = XrdOucUtils::ValPath(cadr, cadr_mode, true)))
      {eMsg  = "Unable to use CA cert directory ";
       eMsg += cadr; eMsg += "; "; eMsg += emsg;
       return false;
      }

// If a ca cert file is present make sure it's a file and only the owner can
// write it (anyone can read it).
//
   if (cafl && (emsg = XrdOucUtils::ValPath(cafl, cafl_mode, false)))
      {eMsg  = "Unable to use CA cert file ";
       eMsg += cafl; eMsg += "; "; eMsg += emsg;
       return false;
      }

// If a private key is present than make sure it's a file and only the
// owner has access to it.
//
   if (pkey && (emsg = XrdOucUtils::ValPath(pkey, pkey_mode, false)))
      {eMsg  = "Unable to use key file ";
       eMsg += pkey; eMsg += "; "; eMsg += emsg;
       return false;
      }

// If a cert file is present then make sure it's a file. If a keyfile is
// present then anyone can read it but only the owner can write it.
// Otherwise, only the owner can gave access to it (it contains the key).
//
   if (cert)
      {mode_t cmode = (pkey ? cert_mode : pkey_mode);
       if ((emsg = XrdOucUtils::ValPath(cert, cmode, false)))
          {if (pkey) eMsg = "Unable to use cert file ";
              else   eMsg = "Unable to use cert+key file ";
           eMsg += cert; eMsg += "; "; eMsg += emsg;
           return false;
          }
      }

// All tests succeeded.
//
   return true;
}

/******************************************************************************/
/*                                 V e r C B                                  */
/******************************************************************************/

extern "C"
{
int VerCB(int aOK, X509_STORE_CTX *x509P)
{
   if (!aOK)
      {X509 *cert = X509_STORE_CTX_get_current_cert(x509P);
       int depth  = X509_STORE_CTX_get_error_depth(x509P);
       int err    = X509_STORE_CTX_get_error(x509P);
       char name[512], info[1024];

       X509_NAME_oneline(X509_get_subject_name(cert), name, sizeof(name));
       snprintf(info,sizeof(info),"Cert verification failed for DN=%s",name);
       XrdTlsGlobal::msgCB("Cert", info, false);

       X509_NAME_oneline(X509_get_issuer_name(cert), name, sizeof(name));
       snprintf(info,sizeof(info),"Failing cert issuer=%s", name);
       XrdTlsGlobal::msgCB("Cert", info, false);

       snprintf(info, sizeof(info), "Error %d at depth %d [%s]", err, depth,
                                    X509_verify_cert_error_string(err));
       XrdTlsGlobal::msgCB("Cert", info, false);
      }

   return aOK;
}
}
  
} // Anonymous namespace end

/******************************************************************************/
/*                           C o n s t r u c t o r                            */
/******************************************************************************/
  
XrdTlsContext::XrdTlsContext(const char *cert,  const char *key,
                             const char *caDir, const char *caFile, int opts) :
      pImpl( new XrdTlsContextImpl() )
{
   class ctx_helper
        {public:

         void Keep() {ctxLoc = 0;}

              ctx_helper(SSL_CTX **ctxP) : ctxLoc(ctxP) {}
             ~ctx_helper() {if (ctxLoc && *ctxLoc)
                               {SSL_CTX_free(*ctxLoc); *ctxLoc = 0;}
                           }
         private:
         SSL_CTX **ctxLoc;
        } ctx_tracker(&pImpl->ctx);

   static const int sslOpts = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3
                            | SSL_OP_NO_COMPRESSION;
   static const int sslMode = SSL_MODE_AUTO_RETRY;

   std::string eText;
   const char *emsg;

// Assume we will fail
//
   pImpl->ctx   = 0;

// Verify that initialzation has occurred. This is not heavy weight as
// there will usually be no more than two instances of this object.
//
   AtomicBeg(ctxMutex);
#if __cplusplus >= 201103L
   bool done = initDone.load();
#else
   bool done = AtomicGet(initDone);
#endif
   AtomicEnd(ctxMutex);
   if (!done && (emsg = Init()))
      {XrdTlsGlobal::msgCB("TLS_Context", emsg, false);
       return;
      }

// If no CA cert information is specified and this is not a server context,
// then get the paths from the environment. They must exist as we need to
// verify peer certs in order to verify target host names client-side. We
// also use this setupt to see if we should use a specific cert and key.
//
   if (!(opts & servr))
      {if (!caDir && !caFile)
          {caDir  = getenv("X509_CERT_DIR");
           caFile = getenv("X509_CERT_FILE");
           if (!caDir && !caFile)
              {XrdTlsGlobal::msgCB("Tls_Context", "Unable to determine the "
                             "location of trusted CA certificates to verify "
                             "peer identify; this is required!", false);
               return;
              }
          }
       if (!cert) cert = getenv("X509_USER_PROXY");
       if (!key)  key  = getenv("X509_USER_KEY");
      }

// Before we try to use any specified files, make sure they exist, are of
// the right type and do not have excessive access privileges.
//
   if (!VerPaths(cert, key, caDir, caFile, eText))
      {XrdTlsGlobal::msgCB("TLS_Context", eText.c_str(), false);
       return;
      }

// Copy parameters to out parm structure.
//
   if (cert)   pImpl->Parm.cert   = cert;
   if (key)    pImpl->Parm.pkey   = key;
   if (caDir)  pImpl->Parm.cadir  = caDir;
   if (caFile) pImpl->Parm.cafile = caFile;
   pImpl->Parm.opts   = opts;

// Get the correct method to use for TLS and check if successful create a
// server context that uses the method.
//
   const SSL_METHOD *meth;
   emsg = GetTlsMethod(meth);
   if (emsg)
      {XrdTlsGlobal::msgCB("TLS_Context", emsg, false);
       return;
      }

   pImpl->ctx = SSL_CTX_new(meth);

// Make sure we have a context here
//
   if (pImpl->ctx == 0)
      {XrdTls::Emsg("TLS_Context", "Unable to allocate TLS context!");
       return;
      }

// Always prohibit SSLv2 & SSLv3 as these are not secure.
//
   SSL_CTX_set_options(pImpl->ctx, sslOpts);

// Handle session re-negotiation automatically
//
   SSL_CTX_set_mode(pImpl->ctx, sslMode);

// Establish the CA cert locations, if specified. Then set the verification
// depth and turn on peer cert validation. For now, we don't set a callback.
// In the future we may to grab debugging information.
//
   if (caDir || caFile)
     {if (!SSL_CTX_load_verify_locations(pImpl->ctx, caFile, caDir))
         {XrdTls::Emsg("TLS_Context",
                       "Unable to set the CA cert file or directory.");
          return;
         }
      int vDepth = (opts & vdept) >> vdepS;
      SSL_CTX_set_verify_depth(pImpl->ctx, (vDepth ? vDepth : 9));

      bool LogVF = (opts & logVF) != 0;
      SSL_CTX_set_verify(pImpl->ctx, SSL_VERIFY_PEER, (LogVF ? VerCB : 0));
     } else {
      SSL_CTX_set_verify(pImpl->ctx, SSL_VERIFY_NONE, 0);
     }

// Set cipher list
//
   if (!SSL_CTX_set_cipher_list(pImpl->ctx, sslCiphers))
      {XrdTls::Emsg("TLS_Context", "Unable to set SSL cipher list.");
       return;
      }

// If we need to enable eliptic-curve support, do so now. Note that for
// OpenSSL 1.1.0+ this is automatically done for us.
//
#if SSL_CTRL_SET_ECDH_AUTO
   SSL_CTX_set_ecdh_auto(pImpl->ctx, 1);
#endif

// The following is for documentation. We normally handle renegotiation during
// reads and writes. The following enables OpenSSL to do that but by blocking.
//
// SSL_CTX_set_mode(pImpl->ctx, SSL_MODE_AUTO_RETRY);

// If there is no cert then assume this is a generic context for a client
//
   if (cert == 0)
      {ctx_tracker.Keep();
       return;
      }

// We have a cert. If the key is missing then we assume the key is in the
// cert file (ssl will complain if it isn't).
//
   if (!key) key = cert;

// Load certificate
//
   if (SSL_CTX_use_certificate_file(pImpl->ctx, cert, SSL_FILETYPE_PEM) != 1)
      {XrdTls::Emsg("LS_Context",
                    "Unable to create TLS context; certificate error.");
       return;
      }

// Load the private key
//
   if (SSL_CTX_use_PrivateKey_file(pImpl->ctx, key, SSL_FILETYPE_PEM) != 1 )
      {XrdTls::Emsg("TLS_Context",
                    "Unable to create TLS context; private key error.");
       return;
      }

// Make sure the key and certificate file match.
//
   if (SSL_CTX_check_private_key(pImpl->ctx) != 1 )
      {XrdTls::Emsg("TLS_Context",
                    "Unable to create TLS context; cert-key mismatch.");
       return;
      }

// All went well, so keep the context.
//
   ctx_tracker.Keep();
}
  
/******************************************************************************/
/*                            D e s t r u c t o r                             */
/******************************************************************************/

XrdTlsContext::~XrdTlsContext() {if (pImpl) delete pImpl;}

/******************************************************************************/
/*                                 C l o n e                                  */
/******************************************************************************/

XrdTlsContext *XrdTlsContext::Clone()
{
  XrdTlsContext::CTX_Params &my = pImpl->Parm;
  const char *cert = (my.cert.size()   ? my.cert.c_str()   : 0);
  const char *pkey = (my.pkey.size()   ? my.pkey.c_str()   : 0);
  const char *caD  = (my.cadir.size()  ? my.cadir.c_str()  : 0);
  const char *caF  = (my.cafile.size() ? my.cafile.c_str() : 0);

// Cloning simply means getting a object with the old parameters.
//
   XrdTlsContext *xtc = new XrdTlsContext(cert, pkey, caD, caF, my.opts);

// Verify that the context was built
//
   if (xtc->Context() != 0) return xtc;

// We failed, cleanup.
//
   delete xtc;
   return 0;
}
  
/******************************************************************************/
/*                               C o n t e x t                                */
/******************************************************************************/

void    *XrdTlsContext::Context()
{
  return pImpl->ctx;
}

/******************************************************************************/
/*                             G e t P a r a m s                              */
/******************************************************************************/

const XrdTlsContext::CTX_Params *XrdTlsContext::GetParams()
{
  return &pImpl->Parm;
}

/******************************************************************************/
/*                                  I n i t                                   */
/******************************************************************************/
  
const char *XrdTlsContext::Init()
{

// Disallow use if this object unless SSL provides thread-safety!
//
#ifndef OPENSSL_THREADS
   return "Installed OpenSSL lacks the required thread support!";
#endif

// Initialize the library (one time call)
//
   InitTLS();
   return 0;
}

/******************************************************************************/
/*                            S e t C i p h e r s                             */
/******************************************************************************/

void XrdTlsContext::SetCiphers(const char *ciphers)
{
   sslCiphers = ciphers;
}
  
/******************************************************************************/
/*                            x 5 0 9 V e r i f y                             */
/******************************************************************************/
  
bool XrdTlsContext::x509Verify()
{
   return !(pImpl->Parm.cadir.empty()) || !(pImpl->Parm.cafile.empty());
}
