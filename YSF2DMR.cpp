/*
*   Copyright (C) 2016,2017 by Jonathan Naylor G4KLX
*   Copyright (C) 2018 by Andy Uribe CA6JAU
*   Copyright (C) 2018 by Manuel Sanchez EA7EE
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "YSF2DMR.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

// "NO GPS" info for DT1 and DT2, suggested by Marius YO2LOJ
const unsigned char dt1_temp[] = {0x34, 0x22, 0x61, 0x5F, 0x28, 0x20, 0x20, 0x20, 0x20, 0x20};
const unsigned char dt2_temp[] = {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x03, 0xE1};

#define DMR_FRAME_PER       55U
#define YSF_FRAME_PER       90U

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "YSF2DMR.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/YSF2DMR.ini";
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>

int end = 0;

#if !defined(_WIN32) && !defined(_WIN64)
void sig_handler(int signo)
{
	if (signo == SIGTERM) {
		end = 1;
		::fprintf(stdout, "Received SIGTERM\n");
	}
}
#endif

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "YSF2DMR version %s\n", VERSION);
				return 0;
			} else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: YSF2DMR [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

#if !defined(_WIN32) && !defined(_WIN64)
	// Capture SIGTERM to finish gracelessly
	if (signal(SIGTERM, sig_handler) == SIG_ERR) 
		::fprintf(stdout, "Can't catch SIGTERM\n");
#endif

	CYSF2DMR* gateway = new CYSF2DMR(std::string(iniFile));

	int ret = gateway->run();

	delete gateway;

	return ret;
}

CYSF2DMR::CYSF2DMR(const std::string& configFile) :
m_callsign(),
m_conf(configFile),
m_dmrNetwork(NULL),
m_dmrLastDT(0U),
m_gps(NULL),
m_dtmf(NULL),
m_APRS(NULL),
m_dmrFrames(0U),
m_ysfFrames(0U)
{
	::memset(m_ysfFrame, 0U, 200U);
	::memset(m_dmrFrame, 0U, 50U);
}

CYSF2DMR::~CYSF2DMR()
{
}

int CYSF2DMR::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "YSF2DMR: cannot read the .ini file\n");
		return 1;
	}

	setlocale(LC_ALL, "C");

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "YSF2DMR: unable to open the log file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::LogWarning("Couldn't fork() , exiting");
			return -1;
		} else if (pid != 0)
			exit(EXIT_SUCCESS);

		// Create new session and process group
		if (::setsid() == -1) {
			::LogWarning("Couldn't setsid(), exiting");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::LogWarning("Couldn't cd /, exiting");
			return -1;
		}

		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);

		//If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::LogError("Could not get the mmdvm user, exiting");
				return -1;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			//Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::LogWarning("Could not set mmdvm GID, exiting");
				return -1;
			}

			if (setuid(mmdvm_uid) != 0) {
				::LogWarning("Could not set mmdvm UID, exiting");
				return -1;
			}

			//Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::LogWarning("It's possible to regain root - something is wrong!, exiting");
				return -1;
			}
		}
	}
#endif

	m_callsign = m_conf.getCallsign();

	bool debug               = m_conf.getDMRNetworkDebug();
	in_addr dstAddress       = CUDPSocket::lookup(m_conf.getDstAddress());
	unsigned int dstPort     = m_conf.getDstPort();
	std::string localAddress = m_conf.getLocalAddress();
	unsigned int localPort   = m_conf.getLocalPort();

	m_ysfNetwork = new CYSFNetwork(localAddress, localPort, m_callsign, debug);
	m_ysfNetwork->setDestination(dstAddress, dstPort);

	ret = m_ysfNetwork->open();
	if (!ret) {
		::LogError("Cannot open the YSF network port");
		::LogFinalise();
		return 1;
	}

	ret = createDMRNetwork();
	if (!ret) {
		::LogError("Cannot open DMR Network");
		::LogFinalise();
		return 1;
	}
	
	std::string lookupFile  = m_conf.getDMRIdLookupFile();
	unsigned int reloadTime = m_conf.getDMRIdLookupTime();

	m_lookup = new CDMRLookup(lookupFile, reloadTime);
	m_lookup->read();

	FLCO dmrflco;
	if (m_dmrpc)
		dmrflco = FLCO_USER_USER;
	else
		dmrflco = FLCO_GROUP;

	CTimer networkWatchdog(100U, 0U, 1500U);
	CTimer pollTimer(1000U, 5U);

	// CWiresX Control Object
	m_wiresX = new CWiresX(m_callsign, "R", m_ysfNetwork);

	std::string name = m_conf.getDescription();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	unsigned int txFrequency = m_conf.getTxFrequency();
	int reflector = m_conf.getDMRDstId();

	m_wiresX->setInfo(name, txFrequency, rxFrequency, reflector);

	m_dtmf = new CDTMF;
	m_APRS = new CAPRSReader(m_conf.getAPRSAPIKey(), m_conf.getAPRSRefresh());

	CStopWatch TGChange;
	CStopWatch stopWatch;
	CStopWatch ysfWatch;
	CStopWatch dmrWatch;
	stopWatch.start();
	ysfWatch.start();
	dmrWatch.start();
	pollTimer.start();

	unsigned char ysf_cnt = 0;
	unsigned char dmr_cnt = 0;

	createGPS();

	LogMessage("Starting YSF2DMR-%s", VERSION);

	bool sendDisconnect = m_conf.getDMRNetworkSendDisconnect();
	bool unlinkReceived = false;

	TG_STATUS TG_connect_state = NONE;
	unsigned char gps_buffer[20U];

	for (; end == 0;) {
		unsigned char buffer[2000U];

		CDMRData tx_dmrdata;
		unsigned int ms = stopWatch.elapsed();

		switch (TG_connect_state) {
			case WAITING_UNLINK:
				if (unlinkReceived) {
					LogMessage("Unlink Received");
					TGChange.start();
					TG_connect_state = SEND_REPLY;
					m_dstid = m_next_dstid;
					unlinkReceived = false;
				}
				break;
			case SEND_REPLY:
				if (TGChange.elapsed() > 600) {
					TGChange.start();
					TG_connect_state = SEND_PTT;
					m_wiresX->sendConnectReply(m_dstid);
				}
				break;
			case SEND_PTT:
				if (TGChange.elapsed() > 600) {
					TGChange.start();
					TG_connect_state = NONE;
					LogMessage("Sending PTT: Src: %s Dst: TG %d", m_ysfSrc.c_str(), m_next_dstid);
					m_srcid = findYSFID(m_ysfSrc);
					SendDummyDMR(m_srcid, m_next_dstid, FLCO_GROUP);
				}
				break;
			default: 
				break;
		}

		if ((TG_connect_state != NONE) && (TGChange.elapsed() > 12000)) {
			LogMessage("Timeout changing TG");
			TG_connect_state = NONE;
		}

		while (m_ysfNetwork->read(buffer) > 0U) {
			CYSFFICH fich;
			bool valid = fich.decode(buffer + 35U);

			if (valid) {
				unsigned char fi = fich.getFI();
				unsigned char dt = fich.getDT();
				unsigned char fn = fich.getFN();
				unsigned char ft = fich.getFT();
				
				WX_STATUS status = m_wiresX->process(buffer + 35U, buffer + 14U, fi, dt, fn, ft);

				switch (status) {
					case WXS_CONNECT:
						m_next_dstid = m_wiresX->getReflector();

						if (m_next_dstid == 9990U)
							dmrflco = FLCO_USER_USER;
						else
							dmrflco = FLCO_GROUP;

						if (m_next_dstid == 4000U)
							continue;

						LogMessage("Connect to %d has been requested by %10.10s", m_next_dstid, buffer + 14U);

						if (sendDisconnect && (m_dstid != 9U)) {
							unsigned char buf[10U];

							::memcpy(buf, buffer + 14U, 6U);
							buf[6U] = 0U;

							m_dstid = 4000U;

							m_ysfSrc = reinterpret_cast<char const*>(buf);
							
							LogMessage("Sending DMR Disconnect: Src: %s Dst: 4000", m_ysfSrc.c_str());

							m_srcid = findYSFID(m_ysfSrc);

							SendDummyDMR(m_srcid, 4000U, FLCO_GROUP);

							unlinkReceived = false;
							TG_connect_state = WAITING_UNLINK;
						} else {
							m_dstid = m_next_dstid;
							TG_connect_state = SEND_REPLY;
						}

						TGChange.start();
						break;

					case WXS_DX:
						break;

					case WXS_DISCONNECT:
						LogMessage("Disconnect has been requested by %10.10s", buffer + 14U);
						unsigned char buf[10U];
						::memcpy(buf, buffer + 14U, 6U);
						buf[6U] = 0U;

						m_dstid = 4000U;
						m_next_dstid = 9U;
						m_ysfSrc = reinterpret_cast<char const*>(buf);
						m_srcid = findYSFID(m_ysfSrc);

						SendDummyDMR(m_srcid, 4000U, FLCO_GROUP);

						TG_connect_state = WAITING_UNLINK;

						TGChange.start();
						break;

					case WXS_NONE:
						if (::memcmp(buffer, "YSFD", 4U) == 0U) {
							CYSFPayload ysfPayload;

							if (dt == YSF_DT_VD_MODE2) {
								if (fi == YSF_FI_HEADER) {
									if (ysfPayload.processHeaderData(buffer + 35U)) {
										std::string ysfSrc = ysfPayload.getSource();
										std::string ysfDst = ysfPayload.getDest();
										LogMessage("Received YSF Header: Src: %s Dst: %s", ysfSrc.c_str(), ysfDst.c_str());
										m_srcid = findYSFID(ysfSrc);
										m_conv.putYSFHeader();
										m_ysfFrames = 0U;
									}
								} else if (fi == YSF_FI_TERMINATOR) {
									LogMessage("YSF received end of voice transmission, %.1f seconds", float(m_ysfFrames) / 10.0F);
									m_conv.putYSFEOT();
									m_ysfFrames = 0U;
								} else if (fi == YSF_FI_COMMUNICATIONS) {
									m_conv.putYSF(buffer + 35U);
									m_ysfFrames++;
								}
							}
						}
						break;

					default:
						break;
				}

				status = WXS_NONE;
				std::string id;

				if (dt == YSF_DT_VD_MODE2)
					status = m_dtmf->decodeVDMode2(buffer + 35U, (buffer[34U] & 0x01U) == 0x01U);

				switch (status) {
					case WXS_CONNECT:
						id = m_dtmf->getReflector();
						m_next_dstid = atoi(id.c_str());
						
						if (m_next_dstid == 4000U)
							continue;
						
						if (m_next_dstid == 9990U)
							dmrflco = FLCO_USER_USER;
						else
							dmrflco = FLCO_GROUP;

						LogMessage("Connect to %d has been requested by %10.10s", m_next_dstid, buffer + 14U);

						if (sendDisconnect && (m_dstid != 9U)) {
							unsigned char buf[10U];
							::memcpy(buf, buffer + 14U, 6U);
							buf[6U] = 0U;

							m_dstid = 4000U;
							m_ysfSrc = reinterpret_cast<char const*>(buf);
							m_srcid = findYSFID(m_ysfSrc);

							LogMessage("Sending DMR Disconnect: Src: %s Dst: 4000", m_ysfSrc.c_str());

							SendDummyDMR(m_srcid, 4000U, FLCO_GROUP);
							
							unlinkReceived = false;
							TG_connect_state = WAITING_UNLINK;
						} else {
							m_dstid = m_next_dstid;
							TG_connect_state = SEND_REPLY;
						}

						TGChange.start();
						break;

					case WXS_DISCONNECT:
						unsigned char buf[10U];
						::memcpy(buf, buffer + 14U, 6U);
						buf[6U] = 0U;

						LogMessage("Disconnect via DTMF has been requested by %10.10s", buffer + 14U);

						m_dstid = 4000U;
						m_next_dstid = 9U;
						m_ysfSrc = reinterpret_cast<char const*>(buf);
						m_srcid = findYSFID(m_ysfSrc);

						SendDummyDMR(m_srcid, 4000U, FLCO_GROUP);

						TG_connect_state = WAITING_UNLINK;
						TGChange.start();
						break;

					default:
						break;
				}

				if (m_gps != NULL)
					m_gps->data(buffer + 14U, buffer + 35U, fi, dt, fn, ft);
				
			}
			
			if ((buffer[34U] & 0x01U) == 0x01U) {
				if (m_gps != NULL)
					m_gps->reset();
				if (m_dtmf != NULL)
					m_dtmf->reset();
			}
		}

		if (dmrWatch.elapsed() > DMR_FRAME_PER) {
			unsigned int dmrFrameType = m_conv.getDMR(m_dmrFrame);

			if(dmrFrameType == TAG_HEADER) {
				CDMRData rx_dmrdata;
				dmr_cnt = 0U;

				rx_dmrdata.setSlotNo(2U);
				rx_dmrdata.setSrcId(m_srcid);
				rx_dmrdata.setDstId(m_dstid);
				rx_dmrdata.setFLCO(dmrflco);
				rx_dmrdata.setN(0U);
				rx_dmrdata.setSeqNo(0U);
				rx_dmrdata.setBER(0U);
				rx_dmrdata.setRSSI(0U);
				rx_dmrdata.setDataType(DT_VOICE_LC_HEADER);

				// Add sync
				CSync::addDMRDataSync(m_dmrFrame, 0);

				// Add SlotType
				CDMRSlotType slotType;
				slotType.setColorCode(m_colorcode);
				slotType.setDataType(DT_VOICE_LC_HEADER);
				slotType.getData(m_dmrFrame);
	
				// Full LC
				CDMRLC dmrLC = CDMRLC(dmrflco, m_srcid, m_dstid);
				CDMRFullLC fullLC;
				fullLC.encode(dmrLC, m_dmrFrame, DT_VOICE_LC_HEADER);
				m_EmbeddedLC.setLC(dmrLC);
				
				rx_dmrdata.setData(m_dmrFrame);
				//CUtils::dump(1U, "DMR data:", m_dmrFrame, 33U);

				for (unsigned int i = 0U; i < 3U; i++) {
					rx_dmrdata.setSeqNo(dmr_cnt);
					m_dmrNetwork->write(rx_dmrdata);
					dmr_cnt++;
				}

				dmrWatch.start();
			}
			else if(dmrFrameType == TAG_EOT) {
				CDMRData rx_dmrdata;
				unsigned int n_dmr = (dmr_cnt - 3U) % 6U;
				unsigned int fill = (6U - n_dmr);
				
				if (n_dmr) {
					for (unsigned int i = 0U; i < fill; i++) {

						CDMREMB emb;
						CDMRData rx_dmrdata;

						rx_dmrdata.setSlotNo(2U);
						rx_dmrdata.setSrcId(m_srcid);
						rx_dmrdata.setDstId(m_dstid);
						rx_dmrdata.setFLCO(dmrflco);
						rx_dmrdata.setN(n_dmr);
						rx_dmrdata.setSeqNo(dmr_cnt);
						rx_dmrdata.setBER(0U);
						rx_dmrdata.setRSSI(0U);
						rx_dmrdata.setDataType(DT_VOICE);

						::memcpy(m_dmrFrame, DMR_SILENCE_DATA, DMR_FRAME_LENGTH_BYTES);

						// Generate the Embedded LC
						unsigned char lcss = m_EmbeddedLC.getData(m_dmrFrame, n_dmr);

						// Generate the EMB
						emb.setColorCode(m_colorcode);
						emb.setLCSS(lcss);
						emb.getData(m_dmrFrame);

						rx_dmrdata.setData(m_dmrFrame);
				
						//CUtils::dump(1U, "DMR data:", m_dmrFrame, 33U);
						m_dmrNetwork->write(rx_dmrdata);

						n_dmr++;
						dmr_cnt++;
					}
				}

				rx_dmrdata.setSlotNo(2U);
				rx_dmrdata.setSrcId(m_srcid);
				rx_dmrdata.setDstId(m_dstid);
				rx_dmrdata.setFLCO(dmrflco);
				rx_dmrdata.setN(n_dmr);
				rx_dmrdata.setSeqNo(dmr_cnt);
				rx_dmrdata.setBER(0U);
				rx_dmrdata.setRSSI(0U);
				rx_dmrdata.setDataType(DT_TERMINATOR_WITH_LC);

				// Add sync
				CSync::addDMRDataSync(m_dmrFrame, 0);

				// Add SlotType
				CDMRSlotType slotType;
				slotType.setColorCode(m_colorcode);
				slotType.setDataType(DT_TERMINATOR_WITH_LC);
				slotType.getData(m_dmrFrame);
	
				// Full LC
				CDMRLC dmrLC = CDMRLC(dmrflco, m_srcid, m_dstid);
				CDMRFullLC fullLC;
				fullLC.encode(dmrLC, m_dmrFrame, DT_TERMINATOR_WITH_LC);
				
				rx_dmrdata.setData(m_dmrFrame);
				//CUtils::dump(1U, "DMR data:", m_dmrFrame, 33U);
				m_dmrNetwork->write(rx_dmrdata);

				dmrWatch.start();
			}
			else if(dmrFrameType == TAG_DATA) {
				CDMREMB emb;
				CDMRData rx_dmrdata;
				unsigned int n_dmr = (dmr_cnt - 3U) % 6U;

				rx_dmrdata.setSlotNo(2U);
				rx_dmrdata.setSrcId(m_srcid);
				rx_dmrdata.setDstId(m_dstid);
				rx_dmrdata.setFLCO(dmrflco);
				rx_dmrdata.setN(n_dmr);
				rx_dmrdata.setSeqNo(dmr_cnt);
				rx_dmrdata.setBER(0U);
				rx_dmrdata.setRSSI(0U);
			
				if (!n_dmr) {
					rx_dmrdata.setDataType(DT_VOICE_SYNC);
					// Add sync
					CSync::addDMRAudioSync(m_dmrFrame, 0U);
					// Prepare Full LC data
					CDMRLC dmrLC = CDMRLC(dmrflco, m_srcid, m_dstid);
					// Configure the Embedded LC
					m_EmbeddedLC.setLC(dmrLC);
				}
				else {
					rx_dmrdata.setDataType(DT_VOICE);
					// Generate the Embedded LC
					unsigned char lcss = m_EmbeddedLC.getData(m_dmrFrame, n_dmr);
					// Generate the EMB
					emb.setColorCode(m_colorcode);
					emb.setLCSS(lcss);
					emb.getData(m_dmrFrame);
				}

				rx_dmrdata.setData(m_dmrFrame);
				
				//CUtils::dump(1U, "DMR data:", m_dmrFrame, 33U);
				m_dmrNetwork->write(rx_dmrdata);

				dmr_cnt++;
				dmrWatch.start();
			}
		}

		while (m_dmrNetwork->read(tx_dmrdata) > 0U) {
			unsigned int SrcId = tx_dmrdata.getSrcId();
			unsigned int DstId = tx_dmrdata.getDstId();
			
			FLCO netflco = tx_dmrdata.getFLCO();
			unsigned char DataType = tx_dmrdata.getDataType();

			if (!tx_dmrdata.isMissing()) {
				networkWatchdog.start();

				if(DataType == DT_TERMINATOR_WITH_LC) {
					LogMessage("DMR received end of voice transmission, %.1f seconds", float(m_dmrFrames) / 16.667F);

					if (SrcId == 4000)
						unlinkReceived = true;

					m_conv.putDMREOT();
					m_dmrNetwork->reset(2U);
					networkWatchdog.stop();
					m_dmrFrames = 0U;
				}

				if((DataType == DT_VOICE_LC_HEADER) && (DataType != m_dmrLastDT)) {
					
					// DT1 & DT2 without GPS info
					::memcpy(gps_buffer, dt1_temp, 10U);
					::memcpy(gps_buffer + 10U, dt2_temp, 10U);

					if (SrcId == 9990U)
						m_netSrc = "PARROT";
					else if (SrcId == 9U)
						m_netSrc = "LOCAL";
					else if (SrcId == 4000U)
						m_netSrc = "UNLINK";
					else
						m_netSrc = m_lookup->findCS(SrcId);

					m_netDst = (netflco == FLCO_GROUP ? "TG " : "") + m_lookup->findCS(DstId);

					m_conv.putDMRHeader();
					LogMessage("DMR Header received from %s to %s", m_netSrc.c_str(), m_netDst.c_str());

					if (m_lookup->exists(SrcId)) {
						int lat, lon, resp;
						resp = m_APRS->findCall(m_netSrc,&lat,&lon);

						//LogMessage("Searching GPS Position of %s in aprs.fi", m_netSrc.c_str());

						if (resp) {
							LogMessage("GPS Position of %s is: lat=%d, lon=%d", m_netSrc.c_str(), lat, lon);
							m_APRS->formatGPS(gps_buffer, lat, lon);
						}
							else LogMessage("GPS Position not available");
					}

					m_netSrc.resize(YSF_CALLSIGN_LENGTH, ' ');
					m_netDst.resize(YSF_CALLSIGN_LENGTH, ' ');
					
					m_dmrFrames = 0U;
				}

				if(DataType == DT_VOICE_SYNC || DataType == DT_VOICE) {
					unsigned char dmr_frame[50];
					tx_dmrdata.getData(dmr_frame);
					m_conv.putDMR(dmr_frame); // Add DMR frame for YSF conversion
					m_dmrFrames++;
				}
			}
			else {
				if(DataType == DT_VOICE_SYNC || DataType == DT_VOICE) {
					unsigned char dmr_frame[50];
					tx_dmrdata.getData(dmr_frame);
					m_conv.putDMR(dmr_frame); // Add DMR frame for YSF conversion
					m_dmrFrames++;
				}

				networkWatchdog.clock(ms);
				if (networkWatchdog.hasExpired()) {
					LogDebug("Network watchdog has expired, %.1f seconds", float(m_dmrFrames) / 16.667F);
					m_dmrNetwork->reset(2U);
					networkWatchdog.stop();
					m_dmrFrames = 0U;
				}
			}
			
			m_dmrLastDT = DataType;
		}
		
		if (ysfWatch.elapsed() > YSF_FRAME_PER) {
			unsigned int ysfFrameType = m_conv.getYSF(m_ysfFrame + 35U);

			if(ysfFrameType == TAG_HEADER) {
				ysf_cnt = 0U;

				::memcpy(m_ysfFrame + 0U, "YSFD", 4U);
				::memcpy(m_ysfFrame + 4U, m_ysfNetwork->getCallsign().c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 14U, m_netSrc.c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 24U, "ALL       ", YSF_CALLSIGN_LENGTH);
				m_ysfFrame[34U] = 0U; // Net frame counter

				CSync::addYSFSync(m_ysfFrame + 35U);

				// Set the FICH
				CYSFFICH fich;
				fich.setFI(YSF_FI_HEADER);
				fich.setCS(2U);
				fich.setFN(0U);
				fich.setFT(7U);
				fich.setDev(0U);
				fich.setMR(2U);
				fich.setDT(YSF_DT_VD_MODE2);
				fich.setSQL(0U);
				fich.setSQ(0U);
				fich.encode(m_ysfFrame + 35U);

				unsigned char csd1[20U], csd2[20U];
				memset(csd1, '*', YSF_CALLSIGN_LENGTH);
				memcpy(csd1 + YSF_CALLSIGN_LENGTH, m_netSrc.c_str(), YSF_CALLSIGN_LENGTH);
				memset(csd2, ' ', YSF_CALLSIGN_LENGTH + YSF_CALLSIGN_LENGTH);

				CYSFPayload payload;
				payload.writeHeader(m_ysfFrame + 35U, csd1, csd2);

				m_ysfNetwork->write(m_ysfFrame);
				
				ysf_cnt++;
				ysfWatch.start();
			}
			else if (ysfFrameType == TAG_EOT) {
				::memcpy(m_ysfFrame + 0U, "YSFD", 4U);
				::memcpy(m_ysfFrame + 4U, m_ysfNetwork->getCallsign().c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 14U, m_netSrc.c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 24U, "ALL       ", YSF_CALLSIGN_LENGTH);
				m_ysfFrame[34U] = ysf_cnt; // Net frame counter

				CSync::addYSFSync(m_ysfFrame + 35U);

				// Set the FICH
				CYSFFICH fich;
				fich.setFI(YSF_FI_TERMINATOR);
				fich.setCS(2U);
				fich.setFN(0U);
				fich.setFT(7U);
				fich.setDev(0U);
				fich.setMR(2U);
				fich.setDT(YSF_DT_VD_MODE2);
				fich.setSQL(0U);
				fich.setSQ(0U);
				fich.encode(m_ysfFrame + 35U);

				unsigned char csd1[20U], csd2[20U];
				memset(csd1, '*', YSF_CALLSIGN_LENGTH);
				memcpy(csd1 + YSF_CALLSIGN_LENGTH, m_netSrc.c_str(), YSF_CALLSIGN_LENGTH);
				memset(csd2, ' ', YSF_CALLSIGN_LENGTH + YSF_CALLSIGN_LENGTH);

				CYSFPayload payload;
				payload.writeHeader(m_ysfFrame + 35U, csd1, csd2);

				m_ysfNetwork->write(m_ysfFrame);
			}
			else if (ysfFrameType == TAG_DATA) {
				CYSFFICH fich;
				CYSFPayload ysfPayload;

				unsigned int fn = (ysf_cnt - 1U) % 8U;

				::memcpy(m_ysfFrame + 0U, "YSFD", 4U);
				::memcpy(m_ysfFrame + 4U, m_ysfNetwork->getCallsign().c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 14U, m_netSrc.c_str(), YSF_CALLSIGN_LENGTH);
				::memcpy(m_ysfFrame + 24U, "ALL       ", YSF_CALLSIGN_LENGTH);

				// Add the YSF Sync
				CSync::addYSFSync(m_ysfFrame + 35U);

				switch (fn) {
					case 0:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, (const unsigned char*)"**********");
						break;
					case 1:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, (const unsigned char*)m_netSrc.c_str());
						break;
					case 2:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, (const unsigned char*)m_netDst.c_str());
						break;
					case 6:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, gps_buffer);
						break;
					case 7:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, gps_buffer+10U);
						break;
					default:
						ysfPayload.writeVDMode2Data(m_ysfFrame + 35U, (const unsigned char*)"          ");
				}
				
				// Set the FICH
				fich.setFI(YSF_FI_COMMUNICATIONS);
				fich.setCS(2U);
				fich.setFN(fn);
				fich.setFT(7U);
				fich.setDev(0U);
				fich.setMR(YSF_MR_BUSY);
				fich.setDT(YSF_DT_VD_MODE2);
				fich.setSQL(0U);
				fich.setSQ(0U);
				fich.encode(m_ysfFrame + 35U);

				// Net frame counter
				m_ysfFrame[34U] = (ysf_cnt & 0x7FU) << 1;

				// Send data to MMDVMHost
				m_ysfNetwork->write(m_ysfFrame);
				
				ysf_cnt++;
				ysfWatch.start();
			}
		}

		stopWatch.start();

		m_ysfNetwork->clock(ms);
		m_dmrNetwork->clock(ms);
		m_wiresX->clock(ms);

		if (m_gps != NULL)
			m_gps->clock(ms);

		pollTimer.clock(ms);
		if (pollTimer.isRunning() && pollTimer.hasExpired()) {
			m_ysfNetwork->writePoll();
			pollTimer.start();
		}

		if (ms < 5U)
			CThread::sleep(5U);
	}

	m_ysfNetwork->close();
	m_dmrNetwork->close();
	m_APRS->stop();
	
	if (m_gps != NULL) {
		m_gps->close();
		delete m_gps;
	}
	
	delete m_dmrNetwork;
	delete m_ysfNetwork;
	delete m_wiresX;
	delete m_dtmf;

	::LogFinalise();

	return 0;
}

void CYSF2DMR::createGPS()
{
	if (!m_conf.getAPRSEnabled())
		return;

	std::string hostname = m_conf.getAPRSServer();
	unsigned int port    = m_conf.getAPRSPort();
	std::string password = m_conf.getAPRSPassword();
	std::string desc     = m_conf.getAPRSDescription();

	LogMessage("APRS Parameters");
	LogMessage("    Server: %s", hostname.c_str());
	LogMessage("    Port: %u", port);
	LogMessage("    Passworwd: %s", password.c_str());
	LogMessage("    Description: %s", desc.c_str());

	m_gps = new CGPS(m_callsign, "R", password, hostname, port);

	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	float latitude           = m_conf.getLatitude();
	float longitude          = m_conf.getLongitude();
	int height               = m_conf.getHeight();

	m_gps->setInfo(txFrequency, rxFrequency, latitude, longitude, height, desc);

	bool ret = m_gps->open();
	if (!ret) {
		delete m_gps;
		LogMessage("Error starting GPS");
		m_gps = NULL;
	}
}

void CYSF2DMR::SendDummyDMR(unsigned int srcid,unsigned int dstid, FLCO dmr_flco)
{
	CDMRData dmrdata;
	CDMRSlotType slotType;
	CDMRFullLC fullLC;

	int dmr_cnt = 0U;

	// Generate DMR LC for header and TermLC frames
	CDMRLC dmrLC = CDMRLC(dmr_flco, srcid, dstid);

	// Build DMR header
	dmrdata.setSlotNo(2U);
	dmrdata.setSrcId(srcid);
	dmrdata.setDstId(dstid);
	dmrdata.setFLCO(dmr_flco);
	dmrdata.setN(0U);
	dmrdata.setSeqNo(0U);
	dmrdata.setBER(0U);
	dmrdata.setRSSI(0U);
	dmrdata.setDataType(DT_VOICE_LC_HEADER);

	// Add sync
	CSync::addDMRDataSync(m_dmrFrame, 0);

	// Add SlotType
	slotType.setColorCode(m_colorcode);
	slotType.setDataType(DT_VOICE_LC_HEADER);
	slotType.getData(m_dmrFrame);

	// Full LC
	fullLC.encode(dmrLC, m_dmrFrame, DT_VOICE_LC_HEADER);

	dmrdata.setData(m_dmrFrame);

	// Send DMR header
	for (unsigned int i = 0U; i < 3U; i++) {
		dmrdata.setSeqNo(dmr_cnt);
		m_dmrNetwork->write(dmrdata);
		dmr_cnt++;
	}

	// Build DMR TermLC
	dmrdata.setSeqNo(dmr_cnt);
	dmrdata.setDataType(DT_TERMINATOR_WITH_LC);

	// Add sync
	CSync::addDMRDataSync(m_dmrFrame, 0);

	// Add SlotType
	slotType.setColorCode(m_colorcode);
	slotType.setDataType(DT_TERMINATOR_WITH_LC);
	slotType.getData(m_dmrFrame);

	// Full LC for TermLC frame
	fullLC.encode(dmrLC, m_dmrFrame, DT_TERMINATOR_WITH_LC);

	dmrdata.setData(m_dmrFrame);

	// Send DMR TermLC
	m_dmrNetwork->write(dmrdata);
}

unsigned int CYSF2DMR::findYSFID(std::string cs)
{
	std::string cstrim;

	int first = cs.find_first_not_of(' ');
	int mid1 = cs.find_last_of('-');
	int mid2 = cs.find_last_of('/');
	int last = cs.find_last_not_of(' ');
	
	LogMessage("ID: first:%d mid1:%d mid2:%d last:%d", first, mid1, mid2, last);

	if (mid1 == -1 && mid2 == -1)
		cstrim = cs.substr(first, (last - first + 1));
	else if (mid1 > first)
		cstrim = cs.substr(first, (mid1 - first));
	else if (mid2 > first)
		cstrim = cs.substr(first, (mid2 - first));
	else
		cstrim = "N0CALL";

	unsigned int id = m_lookup->findID(cstrim);

	if (id == 0) {
		id = m_defsrcid;
		LogMessage("Not DMR ID found, using default ID: %u, DstID: %s %u", id, m_dmrpc ? "" : "TG", m_dstid);
	}
	else
		LogMessage("DMR ID of %s: %u, DstID: %s %u", cstrim.c_str(), id, m_dmrpc ? "" : "TG", m_dstid);

	return id;
}

bool CYSF2DMR::createDMRNetwork()
{
	std::string address  = m_conf.getDMRNetworkAddress();
	unsigned int port    = m_conf.getDMRNetworkPort();
	unsigned int local   = m_conf.getDMRNetworkLocal();
	std::string password = m_conf.getDMRNetworkPassword();
	bool debug           = m_conf.getDMRNetworkDebug();
	unsigned int jitter  = m_conf.getDMRNetworkJitter();
	bool slot1           = false;
	bool slot2           = true;
	bool duplex          = false;
	HW_TYPE hwType       = HWT_MMDVM;

	m_srcHS = m_conf.getDMRId();
	m_colorcode = 1U;
	m_dstid = m_conf.getDMRDstId();
	m_dmrpc = m_conf.getDMRPC();

	if (m_srcHS > 99999999U)
		m_defsrcid = m_srcHS / 100U;
	else if (m_srcHS > 9999999U)
		m_defsrcid = m_srcHS / 10U;
	else
		m_defsrcid = m_srcHS;

	m_srcid = m_defsrcid;
	bool sendDisconnect = m_conf.getDMRNetworkSendDisconnect();
	
	LogMessage("DMR Network Parameters");
	LogMessage("    ID: %u", m_srcHS);
	LogMessage("    Default SrcID: %u", m_defsrcid);
	LogMessage("    Startup DstID: %s %u", m_dmrpc ? "" : "TG", m_dstid);
	LogMessage("    Address: %s", address.c_str());
	LogMessage("    Port: %u", port);
	LogMessage("    Send 4000 Disconect: %s", (sendDisconnect) ? "YES":"NO");
	if (local > 0U)
		LogMessage("    Local: %u", local);
	else
		LogMessage("    Local: random");
	LogMessage("    Jitter: %ums", jitter);

	m_dmrNetwork = new CDMRNetwork(address, port, local, m_srcHS, password, duplex, VERSION, debug, slot1, slot2, hwType, jitter);

	std::string options = m_conf.getDMRNetworkOptions();
	if (!options.empty()) {
		LogMessage("    Options: %s", options.c_str());
		m_dmrNetwork->setOptions(options);
	}

	unsigned int rxFrequency = m_conf.getRxFrequency();
	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int power       = m_conf.getPower();
	float latitude           = m_conf.getLatitude();
	float longitude          = m_conf.getLongitude();
	int height               = m_conf.getHeight();
	std::string location     = m_conf.getLocation();
	std::string description  = m_conf.getDescription();
	std::string url          = m_conf.getURL();

	LogMessage("Info Parameters");
	LogMessage("    Callsign: %s", m_callsign.c_str());
	LogMessage("    RX Frequency: %uHz", rxFrequency);
	LogMessage("    TX Frequency: %uHz", txFrequency);
	LogMessage("    Power: %uW", power);
	LogMessage("    Latitude: %fdeg N", latitude);
	LogMessage("    Longitude: %fdeg E", longitude);
	LogMessage("    Height: %um", height);
	LogMessage("    Location: \"%s\"", location.c_str());
	LogMessage("    Description: \"%s\"", description.c_str());
	LogMessage("    URL: \"%s\"", url.c_str());

	m_dmrNetwork->setConfig(m_callsign, rxFrequency, txFrequency, power, m_colorcode, latitude, longitude, height, location, description, url);

	bool ret = m_dmrNetwork->open();
	if (!ret) {
		delete m_dmrNetwork;
		m_dmrNetwork = NULL;
		return false;
	}

	m_dmrNetwork->enable(true);

	return true;
}
