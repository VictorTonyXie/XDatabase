#pragma once

#include "fileio/FileManager.h"
#include "bufmanager/BufPageManager.h"
#include "utils/pagedef.h"

typedef int RC;
typedef int PageNum;
typedef int SlotNum;

const int MAX_RECORD_SIZE = 1024;//1024 bytes

class RID
{
protected:
	PageNum _pagenum;
	SlotNum _slotnum;

public:
	RID(PageNum pageNum = -1, SlotNum slotNum = -1) :
		_pagenum(pageNum), _slotnum(slotNum) {
		//do nothing
	}
	~RID() {}

	PageNum GetPageNum() const {
		return _pagenum;
	}
	SlotNum GetSlotNum() const {
		return _slotnum;
	}
};

const int SLOT_USED_SIZE = 23;

class RM_FileHead
{
public:
	int recordSize;
	int pageNum;
	int recordNum;
	int maxRecordNum;

	RM_FileHead(int recordSize) :
		recordSize(recordSize),
		pageNum(1),
		recordNum(0) {
		//calculate maxRecordNum
		maxRecordNum = SLOT_USED_SIZE * 8 < (PAGE_SIZE - 96) / recordSize ? SLOT_USED_SIZE * 8 : (PAGE_SIZE - 96) / recordSize;
	}
};

class RM_PageHead
{
public:
	//set ps = slotUsed's size
	//there is ps * 4 + 4 + ps * 4 * 8 * MIN_RECORD_SIZE < 8k
	//so ps <= 227
	//if ps * 4 + 4 == 96 then ps = 23
	int usedSlot;
	unsigned int slotUsed[SLOT_USED_SIZE];

	int getUnusedPosition(int maxRecordNum) {
		//@return: is the slotNum
		int cnt = maxRecordNum < SLOT_USED_SIZE * 32 ? maxRecordNum : SLOT_USED_SIZE * 32;
		for (int i = 0; i < cnt; i++) {
			int k1 = i >> 5;
			int k2 = i % 32;
			int s = slotUsed[k1] & (1 << (31 - k2));
			if (s > 0) return i;
		}
		return 0;
	}

	int getRecordPosition(SlotNum slotNum) {
		int k1 = slotNum >> 5;
		int k2 = slotNum % 32;
		int s = slotUsed[k1] & (1 << (31 - k2));
		return s > 0;
	}

	int setRecordUsed(SlotNum slotNum, bool isUsed) {
		int k1 = slotNum >> 5;
		int k2 = slotNum % 32;
		if (isUsed) {
			slotUsed[k1] |= (1 << (31 - k2));
		}
		else {
			slotUsed[k1] &= ~(1 << (31 - k2));
		}
		return 1;
	}
};

class RM_Record
{
protected:
	//_pdata == nullptr <=> _record_size == -1
	char *_pdata;
	int _record_size;
	RID _rid;
public:
	RM_Record() :
		_pdata(nullptr), _record_size(-1), _rid() {
		/* emtpy */
	}

	~RM_Record() {
		if (_pdata) delete _pdata;
	}

	RM_Record(char *pdata, int recordSize, RID rid) {
		if (pdata == nullptr || recordSize <= 0) {
			return;
		}
		pdata = new char[recordSize + 1];
		memcpy(_pdata, pdata, recordSize);
		_rid = rid;
	}

	RC GetData(char *&pdata) const {
		if (_pdata != nullptr) return 0;
		pdata = _pdata;
		return 1;
	}

	RC GetRid(RID &rid) const {
		if (_pdata != nullptr) return 0;
		rid = _rid;
		return 1;
	}
};

class RM_FileHandle
{
protected:
	BufPageManager *_bpm;
	FileManager *_fm;
	RM_FileHead *_fh;
	int _fileid;
	int _record_size;
public:
	RM_FileHandle() {
		_fm = nullptr;
		_bpm = nullptr;
		_fh = nullptr;
	}

	~RM_FileHandle() {
		if (_bpm) delete _bpm;
	}

	RC OpenFile(FileManager *fm, int fileID) {
		if (fm == nullptr) return 0;
		_fm = fm;
		_bpm = new BufPageManager(fm);
		_fileid = fileID;

	}

	int GetFileID() const {
		return _fileid;
	}

	RC GetRec(const RID &rid, RM_Record &rec) const {
		if (rid.GetPageNum() > _fh->pageNum) return 0;
		int index;
		BufType pageinfo = _bpm->getPage(_fileid, rid.GetPageNum(), index);
		RM_PageHead *pph = (RM_PageHead*)pageinfo;
		int pos = pph->getRecordPosition(rid.GetSlotNum());
		if (pos) {
			rec = RM_Record((char *)pageinfo + sizeof(RM_Record) + _record_size * rid.GetSlotNum(), _record_size, rid);
			return 1;
		}
		return 0;
	}

	RC InsertRec(char *pdata, RID &rid) {
		int res_page = 0, res_slot = 0;
		//search for first position
		int index;
		_fh = (RM_FileHead *)_bpm->getPage(_fileid, 0, index);
		int pagenum = _fh->pageNum;
		int max_record_size = _fh->maxRecordNum;
		for (int i = 1; i <= _fh->pageNum; i++) {
			int index_inside;
			BufType pageinfo = _bpm->getPage(_fileid, i, index_inside);
			RM_PageHead *pph = (RM_PageHead *)pageinfo;
			int pos = pph->getUnusedPosition(max_record_size);
			if (pos > 0) {
				pph->usedSlot++;
				_bpm->markDirty(index_inside);
				pph->setRecordUsed(pos, 1);
				memcpy((char*)pph + 96 + pos * _record_size, pdata, _record_size);
				rid = RID(i, pos);
				return 1;
			}
		}
		//no unused slot!
		//please add new page!
		return 0;
	}

	RC DeleteRec(const RID &rid) {
		int index;
		_fh = (RM_FileHead *)_bpm->getPage(_fileid, 0, index);
		if (_fh->pageNum < rid.GetPageNum() || rid.GetSlotNum() > _fh->maxRecordNum) return 0;
		BufType pageinfo = _bpm->getPage(_fileid, rid.GetPageNum(), index);
		RM_PageHead *pph = (RM_PageHead *)pageinfo;
		pph->setRecordUsed(rid.GetSlotNum(), 0);
		_bpm->markDirty(index);
		return 1;
	}

	RC UpdataRec(const RM_Record &rec) {
		int index;
		_fh = (RM_FileHead *)_bpm->getPage(_fileid, 0, index);
		char *src;
		rec.GetData(src);
		RID rid;
		rec.GetRid(rid);
		if (_fh->pageNum < rid.GetPageNum() || rid.GetSlotNum() > _fh->maxRecordNum) return 0;
		BufType pageinfo = _bpm->getPage(_fileid, rid.GetPageNum(), index);
		RM_PageHead *pph = (RM_PageHead *)pageinfo;
		memcpy((char *)pph + 96 + rid.GetSlotNum() * _record_size, src, _record_size);
		_bpm->markDirty(index);
		return 1;
	}

	/*RC ForcePages(PageNum pageNum = ALL_PAGES) {
		//what is this?
	}*/
};

class RM_Manager
{
protected:
	FileManager *pfm;
	BufPageManager *bpm;
public:
	RM_Manager(FileManager &fm) :
		pfm(&fm), bpm(new BufPageManager(pfm)) {
		//do nothing
	}

	~RM_Manager() {
		if (bpm) delete bpm;
	}

	RC createFile(const char *fileName, int recordSize) {
		if (recordSize > MAX_RECORD_SIZE) return 0;
		bool success = pfm->createFile(fileName);
		if (success) {
			//open fileName and add head-info
			int fileID = 0;
			success = pfm->openFile(fileName, fileID);
			if (!success) return 0;
			int index;
			BufType b = bpm->allocPage(fileID, 0, index, false);
			memset(b, 0, PAGE_SIZE);
			RM_FileHead *pfh = new RM_FileHead(recordSize);
			memcpy(b, pfh, sizeof(RM_FileHead));
			bpm->markDirty(index);
			pfm->closeFile(fileID);
		}
		return success;
	}

	RC DestroyFile(const char *fileName) {
		//no FileManager::DestroyFile function!
		return 0;
	}

	RC OpenFile(const char *fileName, RM_FileHandle &fh) {
		int fileID;
		bool success = pfm->openFile(fileName, fileID);
		if (success) {
			fh.OpenFile(pfm, fileID);
		}
		return success;
	}

	RC CloseFile(RM_FileHandle &fh) {
		int fileID = fh.GetFileID();
		bpm->close();
		pfm->closeFile(fileID);
		return 1;
	}
};
