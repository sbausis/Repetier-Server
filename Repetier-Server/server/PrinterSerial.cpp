/*
 Copyright 2012 Roland Littwin (repetier) repetierdev@gmail.com
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */


#include "PrinterSerial.h"
#include "printer.h"
#include <sys/ioctl.h>
#include <termios.h>
#import <IOKit/serial/ioss.h>

using namespace boost;
using namespace std;

void PrinterSerialPort::set_baudrate(int baud) {
    boost::asio::serial_port_base::baud_rate baudrate = boost::asio::serial_port_base::baud_rate(baud);
    try {
        set_option(baudrate);
    } catch(std::exception e) {
#ifdef __APPLE__
        
        //  boost::asio::detail::reactive_serial_port_service::implementation_type& impl = get_implementation();
        termios ios;
        int handle = (int)native_handle();
        ::tcgetattr(handle, &ios);
        ::cfsetspeed(&ios, baud);
        speed_t newSpeed = baud;
        ioctl(handle, IOSSIOSPEED, &newSpeed);
        ::tcsetattr(native_handle(), TCSANOW, &ios);
        

#else
        cerr << "Setting baudrate " << baudrate.value() << " failed" << endl;
        throw e;
#endif
    }
    
}

PrinterSerial::PrinterSerial(Printer &prt):io(),port(io) {
    open = error = false;
    printer = &prt;
    flowControl = boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none);
    stopBits = boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one);
    parity = asio::serial_port_base::parity(asio::serial_port_base::parity::none);
    characterSize = asio::serial_port_base::character_size(8);
    readString = "";
}
PrinterSerial::~PrinterSerial()
{
    if(isOpen())
    {
        try {
            close();
        } catch(...)
        {
            //Don't throw from a destructor
        }
    }
}
// Returns true if printer is connected
bool PrinterSerial::isConnected() {
    return isOpen();
}
// Tries to connect to printer
bool PrinterSerial::tryConnect() {
    try {
        if(isOpen()) close();
        setErrorStatus(true);//If an exception is thrown, error_ remains true
        baudrate = asio::serial_port_base::baud_rate(printer->baudrate);
        port.open(printer->device);
        port.set_baudrate(printer->baudrate);
 /*       try {
            port.set_option(baudrate);
        } catch(...) {
            cerr << "Setting baudrate " << baudrate.value() << " failed" << endl;
            speed_t newSpeed = printer->baudrate;
            ::cfsetispeed(&port.get_implementation(), printer->baudrate);
            ioctl(port.native_handle(), IOSSIOSPEED, &newSpeed);
        }*/
        port.set_option(parity);
        port.set_option(characterSize);
        port.set_option(flowControl);
        port.set_option(stopBits);
        //This gives some work to the io_service before it is started
        io.post(boost::bind(&PrinterSerial::doRead, this));
        thread t(boost::bind(&asio::io_service::run, &io));
        backgroundThread.swap(t);
        setErrorStatus(false);//If we get here, no error
        open=true; //Port is now open
#ifdef DEBUG
        cout << "Connection started:" << printer->name << endl;
#endif
    } catch (std::exception& e)
    {
#ifdef DEBUG
        // cerr << "Exception: " << e.what() << "\n";
#endif
        return false;
    }
    catch(...) {
        
        return false;
    }
    return true;
}
void PrinterSerial::close() {
    if(!isOpen()) return;
    open=false;
    io.post(boost::bind(&PrinterSerial::doClose, this));
    backgroundThread.join();
    io.reset();
    if(errorStatus())
    {
        throw(boost::system::system_error(boost::system::error_code(),
                                          "Error while closing the device"));
    }
}
bool PrinterSerial::isOpen() {
    return open;
}
bool PrinterSerial::errorStatus() const
{
    lock_guard<mutex> l(errorMutex);
    return error;
}

void PrinterSerial::setErrorStatus(bool e) {
    lock_guard<mutex> l(errorMutex);
    error = e;
}
void PrinterSerial::writeString(const std::string& s)
{
    {
        lock_guard<mutex> l(writeQueueMutex);
        writeQueue.insert(writeQueue.end(),s.begin(),s.end());
    }
    io.post(boost::bind(&PrinterSerial::doWrite, this));
}
void PrinterSerial::writeBytes(const uint8_t* data,size_t len) {
    {
        lock_guard<mutex> l(writeQueueMutex);
        writeQueue.insert(writeQueue.end(),data,data+len);
    }
    io.post(boost::bind(&PrinterSerial::doWrite, this));    
}

void PrinterSerial::doRead() {
    port.async_read_some(asio::buffer(readBuffer,READ_BUFFER_SIZE),
                                boost::bind(&PrinterSerial::readEnd,
                                            this,
                                            asio::placeholders::error,
                                            asio::placeholders::bytes_transferred));
}
void PrinterSerial::readEnd(const boost::system::error_code& error,
                          size_t bytes_transferred)
{
    if(error)
    {
#ifdef __APPLE__
        if(error.value()==45)
        {
            //Bug on OS X, it might be necessary to repeat the setup
            //http://osdir.com/ml/lib.boost.asio.user/2008-08/msg00004.html
            doRead();
            return;
        }
#endif //__APPLE__
        //error can be true even because the serial port was closed.
        //In this case it is not a real error, so ignore
        if(isOpen())
        {
            doClose();
            setErrorStatus(true);
        }
    } else {
        int lstart = 0;
        for(int i=0;i<bytes_transferred;i++) {
            char c = readBuffer[i];
            if(c=='\n' || c=='\r') {
                readString.append(&readBuffer[lstart],i-lstart);
                lstart = i+1;
                if(readString.length()>0)
                    printer->analyseResponse(readString);
                readString = "";
            }
        }
        readString.append(&readBuffer[lstart],bytes_transferred-lstart);
        doRead(); // Continue reading serial port
    }
}
void PrinterSerial::doWrite()
{
    //If a write operation is already in progress, do nothing
    if(writeBuffer==0)
    {
        lock_guard<mutex> l(writeQueueMutex);
        writeBufferSize=writeQueue.size();
        writeBuffer.reset(new char[writeQueue.size()]);
        copy(writeQueue.begin(),writeQueue.end(),
             writeBuffer.get());
        writeQueue.clear();
        async_write(port,asio::buffer(writeBuffer.get(),
                                             writeBufferSize),
                    boost::bind(&PrinterSerial::writeEnd, this, asio::placeholders::error));
    }
}
void PrinterSerial::writeEnd(const boost::system::error_code& error)
{
    if(!error)
    {
        lock_guard<mutex> l(writeQueueMutex);
        if(writeQueue.empty())
        {
            writeBuffer.reset();
            writeBufferSize=0;
            return;
        }
        writeBufferSize=writeQueue.size();
        writeBuffer.reset(new char[writeQueue.size()]);
        copy(writeQueue.begin(),writeQueue.end(),
             writeBuffer.get());
        writeQueue.clear();
        async_write(port,asio::buffer(writeBuffer.get(),
                                             writeBufferSize),
                    boost::bind(&PrinterSerial::writeEnd, this, asio::placeholders::error));
    } else {
        setErrorStatus(true);
        doClose();
    }
}
void PrinterSerial::doClose()
{
    boost::system::error_code ec;
    port.cancel(ec);
    if(ec) setErrorStatus(true);
    port.close(ec);
    if(ec) setErrorStatus(true);
    open = false;
#ifdef DEBUG
    cout << "Connection closed:" << printer->name << endl;
#endif
}

// Send reset to the printer by toggling DTR line
void PrinterSerial::resetPrinter() {
    
}