/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** rtspvideocapturer.h
** 
** -------------------------------------------------------------------------*/

#ifndef RTSPVIDEOCAPTURER_H_
#define RTSPVIDEOCAPTURER_H_

#include <string.h>
#include <vector>

#include "webrtc/media/base/videocapturer.h"
#include "webrtc/base/timeutils.h"


#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

class Callback
{
	public:
		virtual bool notifySession(const char* media, const char* codec) = 0;
		virtual bool notifyData(unsigned char* buffer, ssize_t size) = 0;
};


#define RTSP_CALLBACK(uri, resultCode, resultString) \
static void continueAfter ## uri(RTSPClient* rtspClient, int resultCode, char* resultString) { static_cast<RTSPConnection*>(rtspClient)->continueAfter ## uri(resultCode, resultString); } \
void continueAfter ## uri (int resultCode, char* resultString) \
/**/
u_int8_t marker[] = { 0, 0, 0, 1 }; 
class RTSPConnection : public RTSPClient
{
	class SessionSink: public MediaSink 
	{
		public:
			static SessionSink* createNew(UsageEnvironment& env, Callback* callback) { return new SessionSink(env, callback); }

		private:
			SessionSink(UsageEnvironment& env, Callback* callback) : MediaSink(env), m_bufferSize(1024*1024), m_callback(callback) 
			{
				m_buffer = new u_int8_t[m_bufferSize];
				memcpy(m_buffer, marker, sizeof(marker));
			}
			
			virtual ~SessionSink()
			{
				delete [] m_buffer;
			}

			static void afterGettingFrame(void* clientData, unsigned frameSize,
						unsigned numTruncatedBytes,
						struct timeval presentationTime,
						unsigned durationInMicroseconds)
			{
				static_cast<SessionSink*>(clientData)->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
			}
			
			void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes, struct timeval presentationTime, unsigned durationInMicroseconds)
			{
				this->envir() << "NOTIFY size:" << frameSize << "\n";
				if (numTruncatedBytes != 0)
				{
					delete [] m_buffer;
					m_bufferSize *= 2;
					this->envir() << "buffer too small, reallocate bigger one\n";
					m_buffer = new u_int8_t[m_bufferSize];
					memcpy(m_buffer, marker, sizeof(marker));
				}
				else
				{
					if (!m_callback->notifyData(m_buffer, frameSize+sizeof(marker)))
					{
						this->envir() << "NOTIFY failed\n";
					}
				}
				this->continuePlaying();
			}

		private:
			virtual Boolean continuePlaying()
			{
				Boolean ret = False;
				if (source() != NULL)
				{
					source()->getNextFrame(m_buffer+sizeof(marker), m_bufferSize-sizeof(marker),
							afterGettingFrame, this,
							onSourceClosure, this);
					ret = True;
				}
				return ret;	
			}

		private:
			size_t    m_bufferSize;
			u_int8_t* m_buffer;
			Callback* m_callback; 	
	};
	
	public:
		RTSPConnection(Callback* callback, const std::string & rtspURL, int verbosityLevel = 255) 
						: RTSPClient(*BasicUsageEnvironment::createNew(*BasicTaskScheduler::createNew()), rtspURL.c_str(), verbosityLevel, NULL, 0
#if LIVEMEDIA_LIBRARY_VERSION_INT > 1371168000 
							,-1
#endif
							)
						, m_env(&this->envir())
						, m_session(NULL)
						, m_subSessionIter(NULL)
						, m_callback(callback)
						, m_stop(0)
		{
		}
		
		virtual ~RTSPConnection()
		{
			delete m_subSessionIter;
			Medium::close(m_session);
			TaskScheduler* scheduler = &m_env->taskScheduler();
			m_env->reclaim();
			delete scheduler;
		}
				
		void sendNextCommand() 
		{
			if (m_subSessionIter == NULL)
			{
				this->sendDescribeCommand(continueAfterDESCRIBE); 
			}
			else
			{
				m_subSession = m_subSessionIter->next();
				if (m_subSession != NULL) 
				{
					if (!m_subSession->initiate()) 
					{
						*m_env << "Failed to initiate " << m_subSession->mediumName() << "/" << m_subSession->codecName() << " subsession: " << m_env->getResultMsg() << "\n";
						this->sendNextCommand();
					} 
					else 
					{					
						*m_env << "Initiated " << m_subSession->mediumName() << "/" << m_subSession->codecName() << " subsession\n";
					}

					this->sendSetupCommand(*m_subSession, continueAfterSETUP);
				}
				else
				{
					this->sendPlayCommand(*m_session, continueAfterPLAY);
				}
			}
		}
				
		RTSP_CALLBACK(DESCRIBE,resultCode,resultString)
		{
			if (resultCode != 0) 
			{
				*m_env << "Failed to get a SDP description: " << resultString << "\n";
			}
			else
			{
				char* const sdpDescription = resultString;
				*m_env << "Got a SDP description:\n" << sdpDescription << "\n";
				m_session = MediaSession::createNew(*m_env, sdpDescription);
				m_subSessionIter = new MediaSubsessionIterator(*m_session);
				this->sendNextCommand();  
			}
			delete[] resultString;
		}
		
		RTSP_CALLBACK(SETUP,resultCode,resultString)
		{
			if (resultCode != 0) 
			{
				*m_env << "Failed to SETUP: " << resultString << "\n";
			}
			else
			{				
				m_subSession->sink = SessionSink::createNew(*m_env, m_callback);
				if (m_subSession->sink == NULL) 
				{
					*m_env << "Failed to create a data sink for " << m_subSession->mediumName() << "/" << m_subSession->codecName() << " subsession: " << m_env->getResultMsg() << "\n";
				}
				else if (m_callback->notifySession(m_subSession->mediumName(), m_subSession->codecName()))
				{
					*m_env << "Created a data sink for the \"" << m_subSession << "\" subsession\n";
					m_subSession->sink->startPlaying(*(m_subSession->readSource()), NULL, NULL);
				}
			}
			delete[] resultString;
			this->sendNextCommand();  
		}	
		
		RTSP_CALLBACK(PLAY,resultCode,resultString)
		{
			if (resultCode != 0) 
			{
				*m_env << "Failed to PLAY: " << resultString << "\n";
			}
			else
			{
				*m_env << "PLAY OK " << "\n";
			}
			delete[] resultString;
		}
		
		void mainloop()
		{
			this->sendNextCommand(); 
			m_env->taskScheduler().doEventLoop(&m_stop);
		}
				
		void stop() { m_stop = 1; };
		
	protected:
		UsageEnvironment* m_env;
		MediaSession* m_session;                   
		MediaSubsession* m_subSession;             
		MediaSubsessionIterator* m_subSessionIter;
		Callback* m_callback; 	
		char m_stop;
};


class RTSPVideoCapturer : public cricket::VideoCapturer, public Callback, public rtc::Thread
{
	public:
		RTSPVideoCapturer(const std::string & uri) : m_connection(this,uri.c_str())
		{
			LOG(INFO) << "===========================RTSPVideoCapturer" << uri ;
			
			std::vector<cricket::VideoFormat> formats;
			formats.push_back(cricket::VideoFormat(720, 576, cricket::VideoFormat::FpsToInterval(25), cricket::FOURCC_H264));
			SetSupportedFormats(formats);
		}
	  
		virtual ~RTSPVideoCapturer() 
		{
		}
		
		virtual bool notifySession(const char* media, const char* codec)
		{
			LOG(INFO) << "===========================notifySession" << media << "/" << codec;
		}
		
		virtual bool notifyData(unsigned char* buffer, ssize_t size) 
		{
			std::cout << "===========================notifyData" << size << std::endl;
			if (!IsRunning() || !GetCaptureFormat()) 
			{
				return false;
			}

			cricket::CapturedFrame frame;
			frame.width = GetCaptureFormat()->width;
			frame.height = GetCaptureFormat()->height;
			frame.fourcc = GetCaptureFormat()->fourcc;
			frame.data_size = size;

			std::unique_ptr<char[]> data(new char[size]);
			frame.data = data.get();
			memcpy(frame.data, buffer, size);

			SignalFrameCaptured(this, &frame);
			return true;
		}

		virtual cricket::CaptureState Start(const cricket::VideoFormat& format) 
		{
			SetCaptureFormat(&format);
			SetCaptureState(cricket::CS_RUNNING);
			rtc::Thread::Start();
			return cricket::CS_RUNNING;
		}
	  
		virtual void Stop() 
		{
			m_connection.stop();
			rtc::Thread::Stop();
			SetCaptureFormat(NULL);
			SetCaptureState(cricket::CS_STOPPED);
		}
		
		void Run()
		{	
			m_connection.mainloop();
		}
	  
		virtual bool GetPreferredFourccs(std::vector<unsigned int>* fourccs) 
		{
			fourccs->push_back(cricket::FOURCC_H264);
			return true;
		}
	  
		virtual bool IsScreencast() const { return false; };
		virtual bool IsRunning() { return this->capture_state() == cricket::CS_RUNNING; }
	  
	private:
		RTSPConnection m_connection;
		
};

#endif 
