#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDirIterator>
#include <QCryptographicHash>
#include "sqlite3.h"

//credit goes to Alexander Nusov.
//https://github.com/nusov/qt-crc32
#include "crc32.h"

#include <QDebug>

QByteArray fileChecksum(const QString &fileName, QCryptographicHash::Algorithm hashAlgorithm)
{
    QFile f(fileName);
    if (f.open(QFile::ReadOnly)) {
        QCryptographicHash hash(hashAlgorithm);
        if (hash.addData(&f)) {
            return hash.result();
        }
    }
    return QByteArray();
}

int execsqlquery(sqlite3 *db, QString sqlQuery)
{
    char *zErrMsg = 0;
    int rc = NULL;

    // TODO:  Compile SQL text into byte-code. https://sqlite.org/c3ref/prepare.html

    rc = sqlite3_exec(db, sqlQuery.toLatin1(), 0, 0, &zErrMsg);

    if( rc!=SQLITE_OK )
    {
        qDebug() << QObject::tr("SQL error: ") << zErrMsg;
        sqlite3_free(zErrMsg);
        return 1;
    }
    return rc;
}

bool findsqlrecord(sqlite3 *db, qint64 filesize, QString _id)
{
    if(_id.isEmpty() || _id.isNull())
        return false;

    bool found = false;
    sqlite3_stmt* stmt;

    QString sqlQuery = "SELECT filesize, (filecrc32||filemd5||filesha1) AS checkstr FROM idxfiles WHERE checkstr='" + _id + "' AND filesize=" + QString::number(filesize) + ";";
    //qDebug() << "resulting sql: " << sqlQuery;

    // compile sql statement to binary
    if(sqlite3_prepare_v2(db, sqlQuery.toLatin1(), -1, &stmt, NULL) != SQLITE_OK)
    {
        qDebug() << QObject::tr("ERROR: while compiling sql: ") << sqlite3_errmsg(db);
        sqlite3_close(db);
        sqlite3_finalize(stmt);
        return 1;
    }

    int ret_code = 0;
    while((ret_code = sqlite3_step(stmt)) == SQLITE_ROW) {
        //qDebug() << "idx = " << sqlite3_column_int(stmt, 0);// << "\n";
        found = true;
    }
    if(ret_code != SQLITE_DONE) {
        //this error handling could be done better, but it works
        qDebug() << "ERROR: while performing sql: " << sqlite3_errmsg(db);
        qDebug() << "ret_code = " << ret_code;
        return ret_code;
    }

    //qDebug() << "entry " << (found ? "found" : "not found");
    sqlite3_finalize(stmt);

    return found;
}

QString maindbpath = QDir::tempPath() + "/masterdup.db";
enum eprocess {pnull, pscan, premov, ptest};
eprocess activeproc = pnull;
int main(int argc, char *argv[])
{
    /////////////////////////////////////////////////////////////////////////////////////
    /// START: parse program arguments                                                ///
    /////////////////////////////////////////////////////////////////////////////////////

    QString tmpStr = argv[1];
    if (argc <= 1 || (tmpStr == "-?" || tmpStr == "-h" || tmpStr == "-help" || tmpStr == "--help" || tmpStr == "--?" || tmpStr == "--usage"))
    {
        qDebug() << "fdupfinder: v0.01" << endl << "==============\n"
                 << " -indexdb <path> specify the index database to use.\n"
                 << " -indexdir <path> specify the directory to index.\n"
                 << " -testdir <path> specify the directory to simulate duplicate removal.\n"
                 << " -r       recursive.\n"
                 << " -d       delete duplicate files.\n"
                 << " -l       specify the log file to use.\n"
                 << "use in conjuction with -indexdir:\n"
                 << " -filesize     use filesize.\n"
                 << " -md5          use md5 algorithm.\n"
                 << " -sha1         use sha1 algorithm.\n"
                 << "\n"
                 << "examples: \n"
                 << "fdupfinder -db /tmp/masterdup.db -indexdir /home/user/directory -filesize -md5 -sha1\n";
        return 0;
    }

    bool checkfilesize = false;
    bool checkmd5 = false;
    bool checksha1 = false;
    QDir idxdir;
    QDir rmvdir;
    QDir tstdir;
    int invflag = -1;
    bool flagrecursive = false;
    bool flagdelete = false;

    for (int i = 0; i < argc; i++)
    {
        tmpStr = argv[i];
        if (tmpStr == "-indexdb")
        {
            maindbpath = argv[i + 1];
            continue;
        }
        if (tmpStr == "-indexdir")
        {
            ++invflag;
            activeproc = pscan;
            idxdir.setPath(argv[i + 1]);
            if (!idxdir.exists())
            {
                qDebug() << "invalid index directory. aborting.";
                return 0;
            }
            continue;
        }
        if (tmpStr == "-testdir")
        {
            ++invflag;
            activeproc = ptest;
            tstdir.setPath(argv[i + 1]);
            if (!tstdir.exists())
            {
                qDebug() << "invalid test directory. aborting.";
                return 0;
            }
            continue;
        }
        if (tmpStr == "-r")
        {
            flagrecursive = true;
            continue;
        }
        if (tmpStr == "-d")
        {
            flagdelete = true;
            continue;
        }
        if (tmpStr == "-filesize")
        {
            checkfilesize = true;
            continue;
        }
        if (tmpStr == "-md5")
        {
            checkmd5 = true;
            continue;
        }
        if (tmpStr == "-sha1")
        {
            checksha1 = true;
            continue;
        }
      }

    if (invflag < 0)
    {
        qDebug() << "you cannot combine: indexdir, testdir parameters.";
        return 0;
    }

    /////////////////////////////////////////////////////////////////////////////////////
    /// start main app.                                                               ///
    /////////////////////////////////////////////////////////////////////////////////////

    sqlite3 *maindb = NULL;
    int rc = sqlite3_open(maindbpath.toLatin1(), &maindb);

    if(rc)
    {
        qDebug() << QObject::tr("Can't open database: ") << sqlite3_errmsg(maindb) << "\n";
        sqlite3_close(maindb);
        return(1);
    }

    // sqlite datatypes: https://sqlite.org/datatype3.html
    execsqlquery(maindb, "CREATE TABLE IF NOT EXISTS idxfiles (idx INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE,timestamp INTEGER NOT NULL DEFAULT(strftime('%s', 'now','localtime')),filetype TEXT, filename TEXT, fileext TEXT, filepath TEXT, filesize INTEGER, filecrc32 TEXT, filemd5 TEXT, filesha1 TEXT);");

    Crc32 crc32;
    if (activeproc == pscan)
    {
        QDirIterator::IteratorFlag qiFlags;
        if (flagrecursive)
            qiFlags = QDirIterator::Subdirectories;
        else
            qiFlags = QDirIterator::NoIteratorFlags;

        QDirIterator dirIt(idxdir, qiFlags);
        while (dirIt.hasNext())
        {
            dirIt.next();

            QFileInfo currFile1(dirIt.filePath());
            if (currFile1.isFile() && currFile1.size() > 0)
            {
                if (!findsqlrecord(maindb, currFile1.size(), QString::number(crc32.calculateFromFile(currFile1.filePath()),16)+fileChecksum(currFile1.filePath(), QCryptographicHash::Md5).toHex()+fileChecksum(currFile1.filePath(), QCryptographicHash::Sha1).toHex()))
                {
                    // filetype{d=directory,s=symlink,f=file}
                    qDebug() << "inserting record: " << currFile1.filePath();
                    execsqlquery(maindb, "INSERT INTO idxfiles(filetype,filename,fileext,filepath,filesize,filecrc32,filemd5,filesha1) VALUES('f','" + currFile1.baseName() +"','" +currFile1.completeSuffix()+"','"+currFile1.absolutePath()+"',"+QString::number(currFile1.size())+",'"+QString::number(crc32.calculateFromFile(currFile1.filePath()),16)+"','"+fileChecksum(currFile1.filePath(), QCryptographicHash::Md5).toHex()+"','"+fileChecksum(currFile1.filePath(), QCryptographicHash::Sha1).toHex()+"');");
                } else
                {
                    if (flagdelete && currFile1.isWritable())
                    {
                        qDebug() << "deleting duplicate file: " << currFile1.filePath();
                        QFile delFile1(currFile1.filePath());
                        if (delFile1.remove())
                            qDebug() << "deleted file. ";
                        else
                            qDebug() << "failed to delete file. ";
                    }
                }
            }
        }
    }

    sqlite3_close(maindb);

    qDebug() << "process completed.";
}
