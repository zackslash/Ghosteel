#include "scrollencryptor.h"

#include <cstring>

// Magic header bytes: 'G' 'S' 'B' 0x01
static const char MAGIC[4] = {'G', 'S', 'B', '\x01'};

// PKCS7 padding — implemented manually to avoid plugin-specific behavior.
// Some Sailfish Crypto plugins silently ignore EncryptionPaddingPkcs7.
QByteArray ScrollEncryptor::pkcs7Pad(const QByteArray &data)
{
    int padLen = 16 - (data.size() % 16);
    QByteArray padded = data;
    padded.append(QByteArray(padLen, static_cast<char>(padLen)));
    return padded;
}

QByteArray ScrollEncryptor::pkcs7Unpad(const QByteArray &data)
{
    if (data.isEmpty())
        return data;
    char last = data.at(data.size() - 1);
    int padLen = static_cast<unsigned char>(last);
    if (padLen < 1 || padLen > 16 || padLen > data.size())
        return QByteArray(); // Invalid padding
    // Verify all padding bytes are consistent
    for (int i = data.size() - padLen; i < data.size() - 1; i++) {
        if (data.at(i) != last)
            return QByteArray(); // Invalid padding
    }
    return data.left(data.size() - padLen);
}

bool ScrollEncryptor::isEncryptedFormat(const QByteArray &data)
{
    if (data.size() < 4)
        return false;
    return std::memcmp(data.constData(), MAGIC, 4) == 0;
}

#ifdef SAILFISH_SECRETS

#include <Sailfish/Secrets/secretmanager.h>
#include <Sailfish/Secrets/createcollectionrequest.h>
#include <Sailfish/Secrets/result.h>

#include <Sailfish/Crypto/cryptomanager.h>
#include <Sailfish/Crypto/key.h>
#include <Sailfish/Crypto/generatestoredkeyrequest.h>
#include <Sailfish/Crypto/encryptrequest.h>
#include <Sailfish/Crypto/decryptrequest.h>
#include <Sailfish/Crypto/result.h>

#include <QDebug>
#include <QTimer>
#include <random>

using Sailfish::Secrets::SecretManager;
using Sailfish::Secrets::CreateCollectionRequest;
using Sailfish::Crypto::CryptoManager;
using Sailfish::Crypto::Key;
using Sailfish::Crypto::GenerateStoredKeyRequest;
using Sailfish::Crypto::EncryptRequest;
using Sailfish::Crypto::DecryptRequest;

static const QString COLLECTION_NAME = QStringLiteral("ghosteel");
static const QString KEY_NAME = QStringLiteral("ScrollbackKey");

ScrollEncryptor::ScrollEncryptor(QObject *parent)
    : QObject(parent)
    , m_secretManager(std::make_unique<SecretManager>())
    , m_cryptoManager(std::make_unique<CryptoManager>())
{
    // Defer initialization to next event-loop iteration so the UI can
    // render the first frame before any blocking D-Bus calls.
    m_available = false;
    QTimer::singleShot(0, this, &ScrollEncryptor::initializeAsync);
}

void ScrollEncryptor::initializeNow()
{
    initializeAsync();
}

void ScrollEncryptor::initializeAsync()
{
    // Guard against double-execution: when initializeNow() ran during
    // SessionManager construction, the deferred singleShot becomes a no-op.
    if (m_initialized)
        return;
    m_available = initializeEncryption();
    if (m_available)
        replenishIVs();
    Q_EMIT availabilityChanged();
}

ScrollEncryptor::~ScrollEncryptor() = default;

bool ScrollEncryptor::isAvailable() const
{
    return m_available;
}

bool ScrollEncryptor::initializeEncryption()
{
    if (m_initialized)
        return m_available;
    m_initialized = true;

    if (!ensureCollection()) {
        qWarning() << "Ghosteel: Secrets collection unavailable, "
                      "scrollback encryption disabled";
        return false;
    }

    if (!ensureKey()) {
        qWarning() << "Ghosteel: Stored key unavailable, "
                      "scrollback encryption disabled";
        return false;
    }

    qDebug() << "Ghosteel: Scrollback encryption initialized";
    return true;
}

bool ScrollEncryptor::ensureCollection()
{
    // CreateCollectionRequest is idempotent — succeeds if collection already exists,
    // but returns CollectionAlreadyExistsError on some plugin versions.
    CreateCollectionRequest ccr;
    ccr.setManager(m_secretManager.get());
    ccr.setCollectionName(COLLECTION_NAME);
    ccr.setAccessControlMode(SecretManager::OwnerOnlyMode);
    ccr.setCollectionLockType(CreateCollectionRequest::DeviceLock);
    ccr.setDeviceLockUnlockSemantic(SecretManager::DeviceLockKeepUnlocked);
    ccr.setStoragePluginName(SecretManager::DefaultEncryptedStoragePluginName);
    ccr.setEncryptionPluginName(SecretManager::DefaultEncryptedStoragePluginName);
    ccr.startRequest();
    ccr.waitForFinished();

    if (ccr.result().code() != Sailfish::Secrets::Result::Succeeded) {
        // CollectionAlreadyExists is not a real error — the collection is ready.
        if (ccr.result().errorCode() == Sailfish::Secrets::Result::CollectionAlreadyExistsError)
            return true;

        qWarning() << "Ghosteel: CreateCollection failed:"
                    << ccr.result().errorCode()
                    << ccr.result().errorMessage();
        return false;
    }

    return true;
}

bool ScrollEncryptor::ensureKey()
{
    Key keyTemplate;
    keyTemplate.setAlgorithm(CryptoManager::AlgorithmAes);
    keyTemplate.setSize(256);
    keyTemplate.setOrigin(Key::OriginDevice);
    keyTemplate.setOperations(CryptoManager::OperationEncrypt
                              | CryptoManager::OperationDecrypt);
    keyTemplate.setIdentifier(Key::Identifier(
            KEY_NAME, COLLECTION_NAME,
            CryptoManager::DefaultCryptoStoragePluginName));

    GenerateStoredKeyRequest genKey;
    genKey.setManager(m_cryptoManager.get());
    genKey.setKeyTemplate(keyTemplate);
    genKey.setCryptoPluginName(CryptoManager::DefaultCryptoStoragePluginName);
    genKey.startRequest();
    genKey.waitForFinished();

    if (genKey.result().code() != Sailfish::Crypto::Result::Succeeded) {
        // KeyAlreadyExists — the key was created on a previous launch.
        // Build a reference from the identifier to use it.
        if (genKey.result().errorCode() == Sailfish::Crypto::Result::StorageError
                && genKey.result().errorMessage().contains(QStringLiteral("already exists"))) {
            m_keyReference = std::make_unique<Key>(KEY_NAME, COLLECTION_NAME,
                    CryptoManager::DefaultCryptoStoragePluginName);
            return true;
        }
        qWarning() << "Ghosteel: GenerateStoredKey failed:"
                    << genKey.result().errorCode()
                    << genKey.result().errorMessage();
        return false;
    }

    m_keyReference = std::make_unique<Key>(genKey.generatedKeyReference());
    return true;
}

void ScrollEncryptor::replenishIVs()
{
    // Generate random IVs locally using /dev/urandom (via std::random_device).
    // Avoids D-Bus round-trips and sidesteps GenerateInitializationVectorRequest
    // not being supported on some crypto plugin configurations.
    // Extract 4 bytes per rd() call (32-bit output) instead of wasting 24 bits.
    std::random_device rd;
    while (m_ivPool.size() < IV_POOL_TARGET) {
        QByteArray iv(16, '\0');
        for (int i = 0; i < 16; i += 4) {
            uint32_t val = rd();
            iv[i]     = static_cast<char>((val)       & 0xFF);
            iv[i + 1] = static_cast<char>((val >> 8)  & 0xFF);
            iv[i + 2] = static_cast<char>((val >> 16) & 0xFF);
            iv[i + 3] = static_cast<char>((val >> 24) & 0xFF);
        }
        m_ivPool.append(iv);
    }
}

QByteArray ScrollEncryptor::nextIV()
{
    if (m_ivPool.size() < IV_POOL_THRESHOLD)
        replenishIVs();
    if (m_ivPool.isEmpty())
        return QByteArray();
    return m_ivPool.takeFirst();
}

QByteArray ScrollEncryptor::encrypt(const QByteArray &plaintext)
{
    if (!m_available || !m_keyReference || plaintext.isEmpty())
        return QByteArray();

    const QByteArray iv = nextIV();
    if (iv.isEmpty())
        return QByteArray();

    // PKCS7 pad manually, use EncryptionPaddingNone for the daemon
    const QByteArray padded = pkcs7Pad(plaintext);

    EncryptRequest enc;
    enc.setManager(m_cryptoManager.get());
    enc.setData(padded);
    enc.setInitializationVector(iv);
    enc.setKey(*m_keyReference);
    // CBC without MAC is intentional: device-lock key, bit-flip malleability accepted.
    enc.setBlockMode(CryptoManager::BlockModeCbc);
    enc.setPadding(CryptoManager::EncryptionPaddingNone);
    enc.setCryptoPluginName(CryptoManager::DefaultCryptoStoragePluginName);
    enc.startRequest();
    enc.waitForFinished();

    if (enc.result().code() != Sailfish::Crypto::Result::Succeeded) {
        qWarning() << "Ghosteel: Encryption failed:"
                    << enc.result().errorCode()
                    << enc.result().errorMessage();
        return QByteArray();
    }

    // Build output: magic(4) + reserved(4) + iv(16) + ciphertext
    QByteArray output;
    output.reserve(HEADER_SIZE + enc.ciphertext().size());
    output.append(MAGIC, 4);
    output.append(QByteArray(4, '\x00')); // reserved
    output.append(iv);
    output.append(enc.ciphertext());

    return output;
}

bool ScrollEncryptor::encryptAsync(const QByteArray &plaintext, EncryptCallback callback)
{
    if (!m_available || !m_keyReference || plaintext.isEmpty())
        return false;

    const QByteArray iv = nextIV();
    if (iv.isEmpty())
        return false;

    const QByteArray padded = pkcs7Pad(plaintext);

    // Parented to this for lifetime (auto-delete if destroyed mid-request).
    // The callback runs on the GUI thread because enc is the connect
    // context object and was created here.
    auto *enc = new EncryptRequest(this);
    enc->setManager(m_cryptoManager.get());
    enc->setData(padded);
    enc->setInitializationVector(iv);
    enc->setKey(*m_keyReference);
    enc->setBlockMode(CryptoManager::BlockModeCbc);
    enc->setPadding(CryptoManager::EncryptionPaddingNone);
    enc->setCryptoPluginName(CryptoManager::DefaultCryptoStoragePluginName);

    QObject::connect(enc, &EncryptRequest::statusChanged,
                     enc, [enc, callback, iv](Sailfish::Crypto::Request::Status status) {
        if (status != Sailfish::Crypto::Request::Finished)
            return;
        QByteArray output;
        if (enc->result().code() == Sailfish::Crypto::Result::Succeeded) {
            output.reserve(HEADER_SIZE + enc->ciphertext().size());
            output.append(MAGIC, 4);
            output.append(QByteArray(4, '\x00')); // reserved
            output.append(iv);
            output.append(enc->ciphertext());
        } else {
            qWarning() << "Ghosteel: Async encryption failed:"
                       << enc->result().errorCode()
                       << enc->result().errorMessage();
        }
        callback(output);
        enc->deleteLater();
    });

    enc->startRequest();
    return true;
}

QByteArray ScrollEncryptor::decrypt(const QByteArray &ciphertextWithHeader)
{
    if (!m_available || !m_keyReference)
        return QByteArray();

    if (ciphertextWithHeader.size() < HEADER_SIZE)
        return QByteArray();

    if (!isEncryptedFormat(ciphertextWithHeader))
        return QByteArray();

    const QByteArray iv = ciphertextWithHeader.mid(8, 16);
    const QByteArray ciphertext = ciphertextWithHeader.mid(HEADER_SIZE);

    DecryptRequest dec;
    dec.setManager(m_cryptoManager.get());
    dec.setData(ciphertext);
    dec.setInitializationVector(iv);
    dec.setKey(*m_keyReference);
    dec.setBlockMode(CryptoManager::BlockModeCbc);
    dec.setPadding(CryptoManager::EncryptionPaddingNone);
    dec.setCryptoPluginName(CryptoManager::DefaultCryptoStoragePluginName);
    dec.startRequest();
    dec.waitForFinished();

    if (dec.result().code() != Sailfish::Crypto::Result::Succeeded) {
        qWarning() << "Ghosteel: Decryption failed:"
                    << dec.result().errorCode()
                    << dec.result().errorMessage();
        return QByteArray();
    }

    return pkcs7Unpad(dec.plaintext());
}

#else // !SAILFISH_SECRETS

// Stub implementation for non-Sailfish builds (tests, CI).
// Encryption is unavailable; callers should skip encryption entirely.

#include <QTimer>

ScrollEncryptor::ScrollEncryptor(QObject *parent) : QObject(parent) {
    QTimer::singleShot(0, this, &ScrollEncryptor::initializeAsync);
}
ScrollEncryptor::~ScrollEncryptor() = default;
bool ScrollEncryptor::isAvailable() const { return false; }
QByteArray ScrollEncryptor::encrypt(const QByteArray &) { return QByteArray(); }
bool ScrollEncryptor::encryptAsync(const QByteArray &, EncryptCallback) { return false; }
QByteArray ScrollEncryptor::decrypt(const QByteArray &) { return QByteArray(); }
void ScrollEncryptor::replenishIVs() {}
void ScrollEncryptor::initializeNow() { initializeAsync(); }
void ScrollEncryptor::initializeAsync() { Q_EMIT availabilityChanged(); }

#endif // SAILFISH_SECRETS
