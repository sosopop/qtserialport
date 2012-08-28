/****************************************************************************
**
** Copyright (C) 2012 Denis Shienkov <scapig@yandex.ru>
** Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
** Copyright (C) 2012 Andre Hartmann <aha_1980@gmx.de>
** Contact: http://www.qt-project.org/
**
** This file is part of the QtSerialPort module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

/*!
    \class WinCESerialPortEngine
    \internal

    \brief The WinCESerialPortEngine class provides WindowsCE OS
    platform-specific low level access to a serial port.

    \reentrant
    \ingroup serialport-main
    \inmodule QtSerialPort

    Currently the class supports various versionf of embedded Windows CE.

    WinCESerialPortEngine (as well as other platform-dependent engines)
    is a class with multiple inheritance, which on the one hand,
    derives from a general abstract class interface SerialPortEngine,
    on the other hand from QObject.

    From the abstract class SerialPortEngine, this class inherits all virtual
    interface methods that are common to all serial ports on any platform.
    The class WinCESerialPortEngine implements these methods using the
    Windows CE API.

    For Windows CE systems WinCESerialPortEngine is derived
    from QThread and creates an additional thread to track the events.
*/

#include "serialportengine_wince_p.h"

#include <QtCore/qregexp.h>

#ifndef CTL_CODE
#  define CTL_CODE(DeviceType, Function, Method, Access) ( \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
    )
#endif

#ifndef FILE_DEVICE_SERIAL_PORT
#  define FILE_DEVICE_SERIAL_PORT  27
#endif

#ifndef METHOD_BUFFERED
#  define METHOD_BUFFERED  0
#endif

#ifndef FILE_ANY_ACCESS
#  define FILE_ANY_ACCESS  0x00000000
#endif

#ifndef IOCTL_SERIAL_GET_DTRRTS
#  define IOCTL_SERIAL_GET_DTRRTS \
    CTL_CODE(FILE_DEVICE_SERIAL_PORT, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef SERIAL_DTR_STATE
#  define SERIAL_DTR_STATE  0x00000001
#endif

#ifndef SERIAL_RTS_STATE
#  define SERIAL_RTS_STATE  0x00000002
#endif


QT_BEGIN_NAMESPACE_SERIALPORT

/*!
    Constructs a WinCESerialPortEngine and initializes all internal variables
    to their initial values. The pointer \a d to the private object of class
    SerialPortPrivate is used to call some common methods.
*/
WinCESerialPortEngine::WinCESerialPortEngine(SerialPortPrivate *d)
    : m_descriptor(INVALID_HANDLE_VALUE)
    , m_flagErrorFromCommEvent(false)
    , m_currentMask(0)
    , m_desiredMask(0)
    , m_running(true)
{
    Q_ASSERT(d);
    dptr = d;
    ::memset(&m_currentDcb, 0, sizeof(m_currentDcb));
    ::memset(&m_restoredDcb, 0, sizeof(m_restoredDcb));
    ::memset(&m_currentCommTimeouts, 0, sizeof(m_currentCommTimeouts));
    ::memset(&m_restoredCommTimeouts, 0, sizeof(m_restoredCommTimeouts));
}

/*!
    Stops the serial port event tracking and destructs a WinCESerialPortEngine.
*/
WinCESerialPortEngine::~WinCESerialPortEngine()
{
    m_running = false;
    ::SetCommMask(m_descriptor, 0);
    //terminate();
    wait();
}

/*!
    Attempts to open the desired serial port by \a location in the given open
    \a mode. In the process of discovery, always sets the serial port in
    non-blocking mode (where the read operation returns immediately) and tries
    to determine and install the current configuration to the serial port.

    It should be noted, that Windows has the following limitations when using
    the serial port:
    - support only binary transfers mode
    - always open in exclusive mode

    For Windows NT-based platforms, the serial port is opened in the overlapped
    mode, with the flag FILE_FLAG_OVERLAPPED.

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::open(const QString &location, QIODevice::OpenMode mode)
{
    DWORD desiredAccess = 0;
    DWORD shareMode = 0;
    DWORD flagsAndAttributes = 0;
    bool rxflag = false;
    bool txflag = false;

    if (mode & QIODevice::ReadOnly) {
        desiredAccess |= GENERIC_READ;
        //shareMode = FILE_SHARE_READ;
        rxflag = true;
    }
    if (mode & QIODevice::WriteOnly) {
        desiredAccess |= GENERIC_WRITE;
        //shareMode = FILE_SHARE_WRITE;
        txflag = true;
    }

    // Try opened serial device.
    m_descriptor = ::CreateFile(reinterpret_cast<const wchar_t*>(location.utf16()),
                                desiredAccess, shareMode, 0, OPEN_EXISTING, flagsAndAttributes, 0);

    if (m_descriptor == INVALID_HANDLE_VALUE) {
        dptr->setError(decodeSystemError());
        return false;
    }

    // Save current DCB port settings.
    if (::GetCommState(m_descriptor, &m_restoredDcb) == 0) {
        dptr->setError(decodeSystemError());
        return false;
    }
    m_currentDcb = m_restoredDcb;

    // Set other DCB port options.
    m_currentDcb.fBinary = true;
    m_currentDcb.fInX = false;
    m_currentDcb.fOutX = false;
    m_currentDcb.fAbortOnError = false;
    m_currentDcb.fNull = false;
    m_currentDcb.fErrorChar = false;

    // Apply new DCB init settings.
    if (!updateDcb())
        return false;

    // Save current port timeouts.
    if (::GetCommTimeouts(m_descriptor, &m_restoredCommTimeouts) == 0) {
        dptr->setError(decodeSystemError());
        return false;
    }
    m_currentCommTimeouts = m_restoredCommTimeouts;

    // Set new port timeouts.
    ::memset(&m_currentCommTimeouts, 0, sizeof(m_currentCommTimeouts));
    m_currentCommTimeouts.ReadIntervalTimeout = MAXDWORD;

    // Apply new port timeouts.
    if (!updateCommTimeouts())
        return false;

    detectDefaultSettings();
    return true;
}

/*!
    Closes a serial port. Before closing, restores the previous serial port
    settings if necessary.
*/
void WinCESerialPortEngine::close(const QString &location)
{
    Q_UNUSED(location);

    if (dptr->options.restoreSettingsOnClose) {
        ::SetCommState(m_descriptor, &m_restoredDcb);
        ::SetCommTimeouts(m_descriptor, &m_restoredCommTimeouts);
    }

    ::CloseHandle(m_descriptor);

    m_descriptor = INVALID_HANDLE_VALUE;
}

/*!
    Returns a bitmap state of the RS-232 line signals. On error,
    the bitmap will be empty (equal zero).

    The Windows API only provides the state of the following signals:
    CTS, DSR, RING, DCD, DTR, and RTS. Other signals are not available.
*/
SerialPort::Lines WinCESerialPortEngine::lines() const
{
    DWORD modemStat = 0;
    SerialPort::Lines ret = 0;

    if (::GetCommModemStatus(m_descriptor, &modemStat) == 0)
        return ret;

    if (modemStat & MS_CTS_ON)
        ret |= SerialPort::Cts;
    if (modemStat & MS_DSR_ON)
        ret |= SerialPort::Dsr;
    if (modemStat & MS_RING_ON)
        ret |= SerialPort::Ri;
    if (modemStat & MS_RLSD_ON)
        ret |= SerialPort::Dcd;

    DWORD bytesReturned = 0;
    if (::DeviceIoControl(m_descriptor, IOCTL_SERIAL_GET_DTRRTS, 0, 0,
                          &modemStat, sizeof(bytesReturned),
                          &bytesReturned, 0)) {

        if (modemStat & SERIAL_DTR_STATE)
            ret |= SerialPort::Dtr;
        if (modemStat & SERIAL_RTS_STATE)
            ret |= SerialPort::Rts;
    }

    return ret;
}

/*!
    Set DTR signal to state \a set.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::setDtr(bool set)
{
    return ::EscapeCommFunction(m_descriptor, set ? SETDTR : CLRDTR);
}

/*!
    Set RTS signal to state \a set.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::setRts(bool set)
{
    return ::EscapeCommFunction(m_descriptor, set ? SETRTS : CLRRTS);
}

/*!
    Flushes the serial port's buffers and causes all buffered data to be written
    to the serial port.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::flush()
{
    return ::FlushFileBuffers(m_descriptor);
}

/*!
    Discards all characters from the serial port's output or input buffer.
    This can also terminate pending read or write operations.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::reset()
{
    const DWORD flags = PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR;
    return ::PurgeComm(m_descriptor, flags);
}

/*!
    Sends a continuous stream of zero bits during a specified
    period of time \a duration in msec.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::sendBreak(int duration)
{
    // FIXME:
    if (setBreak(true)) {
        ::Sleep(duration);
        if (setBreak(false))
            return true;
    }
    return false;
}

/*!
    Restores or suspend character transmission and places the
    transmission line in a nonbreak or break state,
    depending on the parameter \a set.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::setBreak(bool set)
{
    if (set)
        return ::SetCommBreak(m_descriptor);
    return ::ClearCommBreak(m_descriptor);
}

/*!
    Returns the number of bytes received by the serial provider
    but not yet read by a read() operation. Also clears the
    device's error flag to enable additional input and output
    (I/O) operations.

    If successful, returns true; otherwise returns false.
*/
qint64 WinCESerialPortEngine::bytesAvailable() const
{
    COMSTAT cs;
    ::memset(&cs, 0, sizeof(cs));
    if (::ClearCommError(m_descriptor, 0, &cs) == 0)
        return -1;
    return cs.cbInQue;
}

/*!
    Returns the number of bytes of user data remaining to be
    transmitted for all write operations. This value will be zero
    for a nonoverlapped write (for embedded platform as WinCE).
    Also clears the device's error flag to enable additional
    input and output (I/O) operations.

    If successful, returns true; otherwise returns false.
*/
qint64 WinCESerialPortEngine::bytesToWrite() const
{
    COMSTAT cs;
    ::memset(&cs, 0, sizeof(cs));
    if (::ClearCommError(m_descriptor, 0, &cs) == 0)
        return -1;
    return cs.cbOutQue;
}

/*!
    Reads at most \a len bytes from the serial port into \a data, and returns
    the number of bytes read. If an error occurs, this function returns -1
    and sets an error code. This function returns immediately.

    Also, this method processed the policy of operating with the
    received symbol, in which the parity or frame error is detected.
    This analysis and processing is executed by software-way in
    this method. Parity or frame error flag determines subsystem
    notification when it receives an event type EV_ERR. Since the
    EV_ERR event appears before the event EV_RXCHAR, therefore,
    we are able to handle errors by ordered, for each bad charachter
    in this read method. This is true only when enabled the internal
    read buffer of class SerialPort, ie when it is automatically
    filled when the notification mode of reading is enabled. In
    other cases, policy processing bad char is not guaranteed.
*/
qint64 WinCESerialPortEngine::read(char *data, qint64 len)
{
    DWORD readBytes = 0;
    bool sucessResult = false;

    // FIXME:
    if (dptr->options.policy != SerialPort::IgnorePolicy)
        len = 1;

    sucessResult = ::ReadFile(m_descriptor, data, len, &readBytes, 0);
    if (!sucessResult)
        return -1;

    // FIXME: Process emulate policy.
    if (m_flagErrorFromCommEvent) {
        m_flagErrorFromCommEvent = false;

        switch (dptr->options.policy) {
        case SerialPort::SkipPolicy:
            return 0;
        case SerialPort::PassZeroPolicy:
            *data = '\0';
            break;
        case SerialPort::StopReceivingPolicy:
            break;
        default:
            break;
        }
    }
    return readBytes;
}

/*!
    Writes at most \a len bytes of data from \a data to the serial port.
    If successful, returns the number of bytes that were actually written;
    otherwise returns -1 and sets an error code.
*/
qint64 WinCESerialPortEngine::write(const char *data, qint64 len)
{
    DWORD writeBytes = 0;
    bool sucessResult = false;

    sucessResult = ::WriteFile(m_descriptor, data, len, &writeBytes, 0);
    if (!sucessResult)
        return -1;

    return writeBytes;
}

/*!
    Implements a function blocking for waiting of events EV_RXCHAR or
    EV_TXEMPTY, on the \a timeout in millisecond. Event EV_RXCHAR
    controlled, if the flag \a checkRead is set on true, and
    EV_TXEMPTY wehn flag \a checkWrite is set on true. The result
    of catch in each of the events, save to the corresponding
    variables \a selectForRead and \a selectForWrite.

    For NT-based OS and Windows CE, this method have different
    implementation. WinCE has no mechanism to exit out of a timeout,
    therefore for this feature special class is used
    WinCeWaitCommEventBreaker, without which it is locked to wait
    forever in the absence of events EV_RXCHAR or EV_TXEMPTY. For
    satisfactory operation of the breaker, the timeout should be
    guaranteed a great, to the timer in the breaker does not trip
    happen sooner than a function call WaitCommEvent(); otherwise it
    will block forever (in the absence of events EV_RXCHAR or EV_TXEMPTY).

    Returns true if the occurrence of any event before the timeout;
    otherwise returns false.
*/
bool WinCESerialPortEngine::select(int timeout,
                                 bool checkRead, bool checkWrite,
                                 bool *selectForRead, bool *selectForWrite)
{
    // FIXME: Forward checking available data for read.
    // This is a bad decision, because call bytesAvailable() automatically
    // clears the error parity, frame, etc. That is, then in the future,
    // it is impossible to identify them in the process of reading the data.
    if (checkRead && (bytesAvailable() > 0)) {
        Q_ASSERT(selectForRead);
        *selectForRead = true;
        return true;
    }

    DWORD oldEventMask = 0;
    DWORD currEventMask = 0;

    if (checkRead)
        currEventMask |= EV_RXCHAR;
    if (checkWrite)
        currEventMask |= EV_TXEMPTY;

    // Save old mask.
    if (::GetCommMask(m_descriptor, &oldEventMask) == 0)
        return false;

    // Checking the old mask bits as in the current mask.
    // And if these bits are not exists, then add them and set the reting mask.
    if (currEventMask != (oldEventMask & currEventMask)) {
        currEventMask |= oldEventMask;
        if (::SetCommMask(m_descriptor, currEventMask) == 0)
            return false;
    }

    currEventMask = 0;
    bool sucessResult = false;

    // FIXME: Here the situation is not properly handled with zero timeout:
    // breaker can work out before you call a method WaitCommEvent()
    // and so it will loop forever!
    WinCeWaitCommEventBreaker breaker(m_descriptor, qMax(timeout, 0));
    ::WaitCommEvent(m_descriptor, &currEventMask, 0);
    breaker.stop();
    sucessResult = !breaker.isWorked();

    if (sucessResult) {
        // FIXME: Here call the bytesAvailable() to protect against false positives
        // WaitForSingleObject(), for example, when manually pulling USB/Serial
        // converter from system, ie when devices are in fact not.
        // While it may be possible to make additional checks - to catch an event EV_ERR,
        // adding (in the code above) extra bits in the mask currEventMask.
        if (checkRead) {
            Q_ASSERT(selectForRead);
            *selectForRead = (currEventMask & EV_RXCHAR) && bytesAvailable() > 0;
        }
        if (checkWrite) {
            Q_ASSERT(selectForWrite);
            *selectForWrite =  currEventMask & EV_TXEMPTY;
        }
    }

    // Rerair old mask.
    ::SetCommMask(m_descriptor, oldEventMask);
    return sucessResult;
}

/*!
    Sets the desired baud \a rate for the given direction \a dir.
    As Windows does not support separate directions, the only valid value for
    \dir is SerialPort::AllDirections.

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::setRate(qint32 rate, SerialPort::Directions dir)
{
    if (dir != SerialPort::AllDirections) {
        dptr->setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }
    m_currentDcb.BaudRate = rate;
    return updateDcb();
}

/*!
    Sets the desired number of data bits \a dataBits in a frame. Windows
    supports all present number of data bits 5, 6, 7, and 8.

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::setDataBits(SerialPort::DataBits dataBits)
{
    m_currentDcb.ByteSize = dataBits;
    return updateDcb();
}

/*!
    Sets the desired \a parity control mode. Windows supports
    all present parity types: no parity, space, mark, even, and odd parity.

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::setParity(SerialPort::Parity parity)
{
    m_currentDcb.fParity = true;
    switch (parity) {
    case SerialPort::NoParity:
        m_currentDcb.Parity = NOPARITY;
        m_currentDcb.fParity = false;
        break;
    case SerialPort::OddParity:
        m_currentDcb.Parity = ODDPARITY;
        break;
    case SerialPort::EvenParity:
        m_currentDcb.Parity = EVENPARITY;
        break;
    case SerialPort::MarkParity:
        m_currentDcb.Parity = MARKPARITY;
        break;
    case SerialPort::SpaceParity:
        m_currentDcb.Parity = SPACEPARITY;
        break;
    default:
        m_currentDcb.Parity = NOPARITY;
        m_currentDcb.fParity = false;
        break;
    }
    return updateDcb();
}

/*!
    Sets the desired number of stop bits \a stopBits in a frame.
    Windows supports 1, 1.5, or 2 stop bits.

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::setStopBits(SerialPort::StopBits stopBits)
{
    switch (stopBits) {
    case SerialPort::OneStop:
        m_currentDcb.StopBits = ONESTOPBIT;
        break;
    case SerialPort::OneAndHalfStop:
        m_currentDcb.StopBits = ONE5STOPBITS;
        break;
    case SerialPort::TwoStop:
        m_currentDcb.StopBits = TWOSTOPBITS;
        break;
    default:
        m_currentDcb.StopBits = ONESTOPBIT;
        break;
    }
    return updateDcb();
}

/*!
    Set desired \a flow control mode. Windows native supported all
    present flow control modes: no control, hardware (RTS/CTS),
    and software (XON/XOFF).

    If successful, returns true; otherwise returns false and sets an
    error code.
*/
bool WinCESerialPortEngine::setFlowControl(SerialPort::FlowControl flow)
{
    m_currentDcb.fInX = false;
    m_currentDcb.fOutX = false;
    m_currentDcb.fOutxCtsFlow = false;
    m_currentDcb.fRtsControl = RTS_CONTROL_DISABLE;
    switch (flow) {
    case SerialPort::NoFlowControl:
        break;
    case SerialPort::SoftwareControl:
        m_currentDcb.fInX = true;
        m_currentDcb.fOutX = true;
        break;
    case SerialPort::HardwareControl:
        m_currentDcb.fOutxCtsFlow = true;
        m_currentDcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
        break;
    default:
        break;
    }
    return updateDcb();
}

/*!
    Empty stub. Setting a variable is carried out methods in a
    private class SerialPortPrivate.
*/
bool WinCESerialPortEngine::setDataErrorPolicy(SerialPort::DataErrorPolicy policy)
{
    Q_UNUSED(policy)
    return true;
}

/*!
    Returns the current read notification subsystem status.
*/
bool WinCESerialPortEngine::isReadNotificationEnabled() const
{
    return isNotificationEnabled(EV_RXCHAR);
}

/*!
    Enables or disables the read notification subsystem, depending on
    the \a enable parameter. The enabled subsystem will asynchronously
    track the occurrence of the event EV_RXCHAR.
    Thereby, SerialPort can emit the signal readyRead() and automatically
    fill the internal receive buffer with new data, that was received from
    the serial port in the event loop.
*/
void WinCESerialPortEngine::setReadNotificationEnabled(bool enable)
{
    setNotificationEnabled(enable, EV_RXCHAR);
}

/*!
    Returns the current write notification subsystem status.
*/
bool WinCESerialPortEngine::isWriteNotificationEnabled() const
{
    return isNotificationEnabled(EV_TXEMPTY);
}

/*!
    Enables or disables the write notification subsystem, depending on
    the \a enable parameter. The enabled subsystem will asynchronously
    track the occurrence of the event EV_TXEMPTY.
    Thereby, SerialPort can automatically write data from the
    internal transfer buffer to the serial port in the event loop.
*/
void WinCESerialPortEngine::setWriteNotificationEnabled(bool enable)
{
    setNotificationEnabled(enable, EV_TXEMPTY);

    // This only for OS Windows, as EV_TXEMPTY event is triggered only
    // after the last byte of data.
    // Therefore, we are forced to run writeNotification(), as EV_TXEMPTY does not work.
    if (enable)
        dptr->canWriteNotification();
}

/*!
    Returns the current error notification subsystem status.
*/
bool WinCESerialPortEngine::isErrorNotificationEnabled() const
{
    return isNotificationEnabled(EV_ERR);
}

/*!
    Enables or disables the error notification subsystem, depending on
    the \a enable parameter. The enabled subsystem will asynchronously
    track the occurrence of an event EV_ERR.
*/
void WinCESerialPortEngine::setErrorNotificationEnabled(bool enable)
{
    setNotificationEnabled(enable, EV_ERR);
}

/*!
    Defines the type of parity or frame error when an event EV_ERR occurs.

    This method is automatically called from the error handler in the
    parent class SerialPortPrivate, which is called by the error notification
    subsystem when an event EV_ERR occurs.
*/
bool WinCESerialPortEngine::processIOErrors()
{
    DWORD err = 0;
    const bool ret = ::ClearCommError(m_descriptor, &err, 0) != 0;
    if (ret && err) {
        if (err & CE_FRAME)
            dptr->setError(SerialPort::FramingError);
        else if (err & CE_RXPARITY)
            dptr->setError(SerialPort::ParityError);
        else if (err & CE_BREAK)
            dptr->setError(SerialPort::BreakConditionError);
        else
            dptr->setError(SerialPort::UnknownPortError);

        m_flagErrorFromCommEvent = true;
    }
    return ret;
}

void WinCESerialPortEngine::lockNotification(NotificationLockerType type, bool uselocker)
{
    QMutex *mutex = 0;
    switch (type) {
    case CanReadLocker:
        mutex = &m_readNotificationMutex;
        break;
    case CanWriteLocker:
        mutex = &m_writeNotificationMutex;
        break;
    case CanErrorLocker:
        mutex = &m_errorNotificationMutex;
        break;
    default:
        break;
    }

    if (uselocker)
        QMutexLocker locker(mutex);
    else
        mutex->lock();
}

void WinCESerialPortEngine::unlockNotification(NotificationLockerType type)
{
    switch (type) {
    case CanReadLocker:
        m_readNotificationMutex.unlock();
        break;
    case CanWriteLocker:
        m_writeNotificationMutex.unlock();
        break;
    case CanErrorLocker:
        m_errorNotificationMutex.unlock();
        break;
    default:
        break;
    }
}

/* Protected methods */

/*!
    Attempts to determine the current serial port settings,
    when the port is opened. Used only in the method open().
*/
void WinCESerialPortEngine::detectDefaultSettings()
{
    // Detect rate.
    dptr->options.inputRate = quint32(m_currentDcb.BaudRate);
    dptr->options.outputRate = dptr->options.inputRate;

    // Detect databits.
    switch (m_currentDcb.ByteSize) {
    case 5:
        dptr->options.dataBits = SerialPort::Data5;
        break;
    case 6:
        dptr->options.dataBits = SerialPort::Data6;
        break;
    case 7:
        dptr->options.dataBits = SerialPort::Data7;
        break;
    case 8:
        dptr->options.dataBits = SerialPort::Data8;
        break;
    default:
        dptr->options.dataBits = SerialPort::UnknownDataBits;
        break;
    }

    // Detect parity.
    if ((m_currentDcb.Parity == NOPARITY) && !m_currentDcb.fParity)
        dptr->options.parity = SerialPort::NoParity;
    else if ((m_currentDcb.Parity == SPACEPARITY) && m_currentDcb.fParity)
        dptr->options.parity = SerialPort::SpaceParity;
    else if ((m_currentDcb.Parity == MARKPARITY) && m_currentDcb.fParity)
        dptr->options.parity = SerialPort::MarkParity;
    else if ((m_currentDcb.Parity == EVENPARITY) && m_currentDcb.fParity)
        dptr->options.parity = SerialPort::EvenParity;
    else if ((m_currentDcb.Parity == ODDPARITY) && m_currentDcb.fParity)
        dptr->options.parity = SerialPort::OddParity;
    else
        dptr->options.parity = SerialPort::UnknownParity;

    // Detect stopbits.
    switch (m_currentDcb.StopBits) {
    case ONESTOPBIT:
        dptr->options.stopBits = SerialPort::OneStop;
        break;
    case ONE5STOPBITS:
        dptr->options.stopBits = SerialPort::OneAndHalfStop;
        break;
    case TWOSTOPBITS:
        dptr->options.stopBits = SerialPort::TwoStop;
        break;
    default:
        dptr->options.stopBits = SerialPort::UnknownStopBits;
        break;
    }

    // Detect flow control.
    if (!m_currentDcb.fOutxCtsFlow && (m_currentDcb.fRtsControl == RTS_CONTROL_DISABLE)
            && !m_currentDcb.fInX && !m_currentDcb.fOutX) {
        dptr->options.flow = SerialPort::NoFlowControl;
    } else if (!m_currentDcb.fOutxCtsFlow && (m_currentDcb.fRtsControl == RTS_CONTROL_DISABLE)
               && m_currentDcb.fInX && m_currentDcb.fOutX) {
        dptr->options.flow = SerialPort::SoftwareControl;
    } else if (m_currentDcb.fOutxCtsFlow && (m_currentDcb.fRtsControl == RTS_CONTROL_HANDSHAKE)
               && !m_currentDcb.fInX && !m_currentDcb.fOutX) {
        dptr->options.flow = SerialPort::HardwareControl;
    } else
        dptr->options.flow = SerialPort::UnknownFlowControl;
}

/*!
    Converts the platform-depend code of system error to the
    corresponding value a SerialPort::PortError.
*/
SerialPort::PortError WinCESerialPortEngine::decodeSystemError() const
{
    SerialPort::PortError error;
    switch (::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
        error = SerialPort::NoSuchDeviceError;
        break;
    case ERROR_ACCESS_DENIED:
        error = SerialPort::PermissionDeniedError;
        break;
    case ERROR_INVALID_HANDLE:
        error = SerialPort::DeviceIsNotOpenedError;
        break;
    case ERROR_INVALID_PARAMETER:
        error = SerialPort::UnsupportedPortOperationError;
        break;
    default:
        error = SerialPort::UnknownPortError;
        break;
    }
    return error;
}

/*!
    Embedded-based (WinCE) event loop for the notification subsystem.
    The serial port events EV_ERR, EV_RXCHAR, and EV_TXEMPTY are tracked
    in a separate thread. When a relevant event occurs, the appropriate
    handler from the parent class SerialPortPrivate is called.
    At the same time in handlers to capture/release the mutex
    (see handlers implementation).
*/
void WinCESerialPortEngine::run()
{
    while (m_running) {

        m_setCommMaskMutex.lock();
        ::SetCommMask(m_descriptor, m_desiredMask);
        m_setCommMaskMutex.unlock();

        if (::WaitCommEvent(m_descriptor, &m_currentMask, 0) != 0) {

            // Wait until complete the operation changes the port settings,
            // see updateDcb().
            m_settingsChangeMutex.lock();
            m_settingsChangeMutex.unlock();

            if (EV_ERR & m_currentMask & m_desiredMask) {
                dptr->canErrorNotification();
            }
            if (EV_RXCHAR & m_currentMask & m_desiredMask) {
                dptr->canReadNotification();
            }
            //FIXME: This is why it does not work?
            if (EV_TXEMPTY & m_currentMask & m_desiredMask) {
                dptr->canWriteNotification();
            }
        }
    }
}

/*!
*/
bool WinCESerialPortEngine::isNotificationEnabled(DWORD mask) const
{
    bool enabled = isRunning();
    return enabled && (m_desiredMask & mask);
}

/*!
*/
void WinCESerialPortEngine::setNotificationEnabled(bool enable, DWORD mask)
{
    m_setCommMaskMutex.lock();
    ::GetCommMask(m_descriptor, &m_currentMask);

    // Mask only the desired bits without affecting others.
    if (enable)
        m_desiredMask |= mask;
    else
        m_desiredMask &= ~mask;

    ::SetCommMask(m_descriptor, m_desiredMask);

    m_setCommMaskMutex.unlock();

    if (enable && !isRunning())
        start();
}

/*!
    Updates the DCB structure when changing any serial port parameter.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::updateDcb()
{
    // Grab a mutex, in order after exit WaitCommEvent
    // block the flow of run() notifier until there is a change DCB.
    QMutexLocker locker(&m_settingsChangeMutex);
    // This way, we reset in class WaitCommEvent to
    // be able to change the DCB.
    // Otherwise WaitCommEvent blocking any change!
    ::SetCommMask(m_descriptor, 0);
    if (::SetCommState(m_descriptor, &m_currentDcb) == 0) {
        dptr->setError(decodeSystemError());
        return false;
    }
    return true;
}

/*!
    Updates the COMMTIMEOUTS structure when changing any serial port
    timeout parameter.

    If successful, returns true; otherwise returns false.
*/
bool WinCESerialPortEngine::updateCommTimeouts()
{
    if (::SetCommTimeouts(m_descriptor, &m_currentCommTimeouts) == 0) {
        dptr->setError(decodeSystemError());
        return false;
    }
    return true;
}

// From <serialportengine_p.h>
SerialPortEngine *SerialPortEngine::create(SerialPortPrivate *d)
{
    return new WinCESerialPortEngine(d);
}

/* Public static the SerialPortPrivate methods */

static const QLatin1String defaultPathPostfix(":");

/*!
    Converts a platform specific \a port name to a system location
    and returns the value.
*/
QString SerialPortPrivate::portNameToSystemLocation(const QString &port)
{
    QString ret = port;
    if (!ret.contains(defaultPathPostfix))
        ret.append(defaultPathPostfix);
    return ret;
}

/*!
    Converts a platform specific system \a location to a port name
    and returns the value.
*/
QString SerialPortPrivate::portNameFromSystemLocation(const QString &location)
{
    QString ret = location;
    if (ret.contains(defaultPathPostfix))
        ret.remove(defaultPathPostfix);
    return ret;
}

// This table contains standard values of baud rates that
// are defined in MSDN and/or in Win SDK file winbase.h
static
const qint32 standardRatesTable[] =
{
    #ifdef CBR_110
    CBR_110,
    #endif
    #ifdef CBR_300
    CBR_300,
    #endif
    #ifdef CBR_600
    CBR_600,
    #endif
    #ifdef CBR_1200
    CBR_1200,
    #endif
    #ifdef CBR_2400
    CBR_2400,
    #endif
    #ifdef CBR_4800
    CBR_4800,
    #endif
    #ifdef CBR_9600
    CBR_9600,
    #endif
    #ifdef CBR_14400
    CBR_14400,
    #endif
    #ifdef CBR_19200
    CBR_19200,
    #endif
    #ifdef CBR_38400
    CBR_38400,
    #endif
    #ifdef CBR_56000
    CBR_56000,
    #endif
    #ifdef CBR_57600
    CBR_57600,
    #endif
    #ifdef CBR_115200
    CBR_115200,
    #endif
    #ifdef CBR_128000
    CBR_128000,
    #endif
    #ifdef CBR_256000
    CBR_256000
    #endif
};

static const qint32 *standardRatesTable_end =
        standardRatesTable + sizeof(standardRatesTable)/sizeof(*standardRatesTable);

/*!
    Converts the windows-specific baud rate code \a setting to a numeric value.
    If the desired item is not found, returns 0.
*/
qint32 SerialPortPrivate::rateFromSetting(qint32 setting)
{
    const qint32 *ret = qFind(standardRatesTable, standardRatesTable_end, setting);
    return ret != standardRatesTable_end ? *ret : 0;
}

/*!
    Converts a numeric baud \a rate value to the windows-specific code.
    If the desired item is not found, returns 0.
*/
qint32 SerialPortPrivate::settingFromRate(qint32 rate)
{
    const qint32 *ret = qBinaryFind(standardRatesTable, standardRatesTable_end, rate);
    return ret != standardRatesTable_end ? *ret : 0;
}

/*!
   Returns a list of standard baud rates that
   are defined in MSDN and/or in Win SDK file winbase.h.
*/
QList<qint32> SerialPortPrivate::standardRates()
{
   QList<qint32> l;
   for (const qint32 *it = standardRatesTable; it != standardRatesTable_end; ++it)
      l.append(*it);
   return l;
}

#include "moc_serialportengine_wince_p.cpp"

QT_END_NAMESPACE_SERIALPORT
