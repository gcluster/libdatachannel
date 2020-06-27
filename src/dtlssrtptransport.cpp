/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "dtlssrtptransport.hpp"
#include "tls.hpp"

#if RTC_ENABLE_MEDIA

#include <cstring>
#include <exception>

using std::shared_ptr;
using std::to_integer;
using std::to_string;

namespace rtc {

void DtlsSrtpTransport::Init() { srtp_init(); }

void DtlsSrtpTransport::Cleanup() { srtp_shutdown(); }

DtlsSrtpTransport::DtlsSrtpTransport(std::shared_ptr<IceTransport> lower,
                                     shared_ptr<Certificate> certificate,
                                     verifier_callback verifierCallback,
                                     message_callback srtpRecvCallback,
                                     state_callback stateChangeCallback)
    : DtlsTransport(lower, certificate, std::move(verifierCallback),
                    std::move(stateChangeCallback)),
      mSrtpRecvCallback(std::move(srtpRecvCallback)) { // distinct from Transport recv callback

	PLOG_DEBUG << "Initializing DTLS-SRTP transport";

	if (srtp_err_status_t err = srtp_create(&mSrtpIn, nullptr)) {
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
	}
	if (srtp_err_status_t err = srtp_create(&mSrtpOut, nullptr)) {
		srtp_dealloc(mSrtpIn);
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
	}
}

DtlsSrtpTransport::~DtlsSrtpTransport() {
	stop();

	srtp_dealloc(mSrtpIn);
	srtp_dealloc(mSrtpOut);
}

bool DtlsSrtpTransport::sendMedia(message_ptr message) {
	if (!message)
		return false;

	if (!mInitDone) {
		PLOG_WARNING << "SRTP media sent before keys are derived";
		return false;
	}

	int size = message->size();
	PLOG_VERBOSE << "Send size=" << size;

	// srtp_protect() assumes that it can write SRTP_MAX_TRAILER_LEN (for the authentication tag)
	// into the location in memory immediately following the RTP packet.
	message->resize(size + SRTP_MAX_TRAILER_LEN);
	if (srtp_err_status_t err = srtp_protect(mSrtpOut, message->data(), &size)) {
		if (err == srtp_err_status_replay_fail)
			throw std::runtime_error("SRTP packet is a replay");
		else
			throw std::runtime_error("SRTP protect error, status=" +
			                         to_string(static_cast<int>(err)));
	}
	PLOG_VERBOSE << "Protected SRTP packet, size=" << size;
	message->resize(size);
	outgoing(message);
	return true;
}

void DtlsSrtpTransport::incoming(message_ptr message) {
	if (!mInitDone) {
		// Bypas
		DtlsTransport::incoming(message);
		return;
	}

	int size = message->size();
	if (size == 0)
		return;

	// RFC 5764 5.1.2. Reception
	// The process for demultiplexing a packet is as follows. The receiver looks at the first byte
	// of the packet. [...] If the value is in between 128 and 191 (inclusive), then the packet is
	// RTP (or RTCP [...]). If the value is between 20 and 63 (inclusive), the packet is DTLS.
	uint8_t value = to_integer<uint8_t>(*message->begin());

	PLOG_VERBOSE << "Demultiplexing packet with first byte, value=" << unsigned(value);

	if (value >= 128 && value <= 191) {
		PLOG_VERBOSE << "Incoming DTLS packet, size=" << size;
		DtlsTransport::incoming(message);

	} else if (value >= 20 && value <= 63) {
		PLOG_VERBOSE << "Incoming SRTP packet, size=" << size;

		if (srtp_err_status_t err = srtp_unprotect(mSrtpIn, message->data(), &size)) {
			if (err == srtp_err_status_replay_fail)
				PLOG_WARNING << "Incoming SRTP packet is a replay";
			else
				PLOG_WARNING << "SRTP unprotect error, status=" << err;
			return;
		}
		PLOG_VERBOSE << "Unprotected SRTP packet, size=" << size;
		message->resize(size);
		mSrtpRecvCallback(message);

	} else {
		PLOG_WARNING << "Unknown packet type, value=" << unsigned(value) << ", size=" << size;
	}
}

void DtlsSrtpTransport::postCreation() {
#if USE_GNUTLS
	PLOG_DEBUG << "Setting SRTP profile (GnuTLS)";
	gnutls::check(gnutls_srtp_set_profile(mSession, GNUTLS_SRTP_AES128_CM_HMAC_SHA1_80),
	              "Failed to set SRTP profile");
#else
	PLOG_DEBUG << "Setting SRTP profile (OpenSSL)";
	// returns 0 on success, 1 on error
	if (SSL_set_tlsext_use_srtp(mSsl, "SRTP_AES128_CM_SHA1_80"), "Failed to set SRTP profile")
		throw std::runtime_error("Failed to set SRTP profile: " + openssl::error_string(ERR_get_error()));
#endif
}

void DtlsSrtpTransport::postHandshake() {
	if (mInitDone)
		return;

	const size_t materialLen = SRTP_AES_ICM_128_KEY_LEN_WSALT * 2;
	unsigned char material[materialLen];
	const unsigned char *clientKey, *clientSalt, *serverKey, *serverSalt;

#if USE_GNUTLS
	PLOG_INFO << "Deriving SRTP keying material (GnuTLS)";

	gnutls_datum_t clientKeyDatum, clientSaltDatum, serverKeyDatum, serverSaltDatum;
	gnutls::check(gnutls_srtp_get_keys(mSession, material, materialLen, &clientKeyDatum,
	                                   &clientSaltDatum, &serverKeyDatum, &serverSaltDatum),
	              "Failed to derive SRTP keys");

	if (clientKeyDatum.size != SRTP_AES_128_KEY_LEN)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(clientKeyDatum.size));
	if (clientSaltDatum.size != SRTP_SALT_LEN)
		throw std::logic_error("Unexpected SRTP salt length: " + to_string(clientSaltDatum.size));
	if (serverKeyDatum.size != SRTP_AES_128_KEY_LEN)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(serverKeyDatum.size));
	if (serverSaltDatum.size != SRTP_SALT_LEN)
		throw std::logic_error("Unexpected SRTP salt size: " + to_string(serverSaltDatum.size));

	clientKey = reinterpret_cast<const unsigned char *>(clientKeyDatum.data);
	clientSalt = reinterpret_cast<const unsigned char *>(clientSaltDatum.data);

	serverKey = reinterpret_cast<const unsigned char *>(serverKeyDatum.data);
	serverSalt = reinterpret_cast<const unsigned char *>(serverSaltDatum.data);
#else
	PLOG_INFO << "Deriving SRTP keying material (OpenSSL)";

	// The extractor provides the client write master key, the server write master key, the client
	// write master salt and the server write master salt in that order.
	const string label = "EXTRACTOR-dtls_srtp";

	// returns 1 on success, 0 or -1 on failure (OpenSSL API is a complete mess...)
	if (SSL_export_keying_material(mSsl, material, materialLen, label.c_str(), label.size(),
	                               nullptr, 0, 0) <= 0)
		throw std::runtime_error("Failed to derive SRTP keys: " +
		                         openssl::error_string(ERR_get_error()));

	clientKey = material;
	clientSalt = clientKey + SRTP_AES_128_KEY_LEN;

	serverKey = material + SRTP_AES_ICM_128_KEY_LEN_WSALT;
	serverSalt = serverKey + SRTP_AES_128_KEY_LEN;
#endif

	unsigned char clientSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	std::memcpy(clientSessionKey, clientKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(clientSessionKey + SRTP_AES_128_KEY_LEN, clientSalt, SRTP_SALT_LEN);

	unsigned char serverSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	std::memcpy(serverSessionKey, serverKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(serverSessionKey + SRTP_AES_128_KEY_LEN, serverSalt, SRTP_SALT_LEN);

	srtp_policy_t inbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtcp);
	inbound.ssrc.type = ssrc_any_inbound;
	inbound.ssrc.value = 0;
	inbound.key = mIsClient ? serverSessionKey : clientSessionKey;
	inbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpIn, &inbound))
		throw std::runtime_error("SRTP add inbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	srtp_policy_t outbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtcp);
	outbound.ssrc.type = ssrc_any_outbound;
	outbound.ssrc.value = 0;
	outbound.key = mIsClient ? clientSessionKey : serverSessionKey;
	outbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpOut, &outbound))
		throw std::runtime_error("SRTP add outbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	mInitDone = true;
}

} // namespace rtc

#endif
