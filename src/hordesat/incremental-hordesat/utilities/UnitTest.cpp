/*
 * UnitTest.cpp
 *
 *  Created on: Oct 15, 2014
 *      Author: balyo
 */

#include "ClauseDatabase.h"
#include "BufferManager.h"
#include "ClauseFilter.h"
#include "ClauseManager.h"
#include <stdarg.h>
#include "DebugUtils.h"
#include <algorithm>
#include <string.h>
#include <set>
#include "Logger.h"
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>

vector<int> makecls(int l) {
	vector<int> vec;
	vec.push_back(l);
	return vec;
}

vector<int> makecls(int l1, int l2) {
	vector<int> vec;
	vec.push_back(l1);
	vec.push_back(l2);
	return vec;
}

vector<int> makecls(int l1, int l2, int l3) {
	vector<int> vec;
	vec.push_back(l1);
	vec.push_back(l2);
	vec.push_back(l3);
	return vec;
}

vector<int> makecls(int l1, int l2, int l3, int l4) {
	vector<int> vec;
	vec.push_back(l1);
	vec.push_back(l2);
	vec.push_back(l3);
	vec.push_back(l4);
	return vec;
}

vector<int> makeRandomCls(int size, int vars) {
	vector<int> vec;
	for (int i = 0; i < size; i++) {
		int var = 1 + (rand() % vars);
		int sgn = (rand() % 2 == 0) ? 1 : -1;
		vec.push_back(var*sgn);
	}
	return vec;
}

void testClauseDatabaseManual() {
	ClauseDatabase cdb1,cdb2,cdb3;
	vector<int> cls = makecls(1,2,3); cdb1.addClause(cls);
	cls = makecls(11,22); cdb1.addClause(cls);
	cls = makecls(1,-8,3); cdb1.addClause(cls);
	cls = makecls(-1); cdb1.addClause(cls);
	cls = makecls(-3); cdb1.addClause(cls);
	cls = makecls(7,8); cdb1.addClause(cls);
	cls = makecls(9,11); cdb1.addClause(cls);

	cls = makecls(99,98,97); cdb1.addVIPClause(cls);
	cls = makecls(4,5); cdb1.addVIPClause(cls);

	cls = makecls(-7,-8); cdb2.addClause(cls);
	cls = makecls(19,-111); cdb2.addClause(cls);
	cls = makecls(4,7,8); cdb2.addClause(cls);

	cls = makecls(99); cdb2.addVIPClause(cls);
	cls = makecls(-3,-9); cdb2.addVIPClause(cls);

	cls = makecls(100); cdb3.addClause(cls);
	cls = makecls(11, 23, -111); cdb3.addClause(cls);
	cls = makecls(7,8,9); cdb3.addClause(cls);
	cls = makecls(7,8,9,10); cdb3.addClause(cls);
	cls = makecls(99,98,97); cdb3.addVIPClause(cls);
	cls = makecls(1,2); cdb3.addVIPClause(cls);

	int nodes = 3;
	int size = 15;
	int* buffer = new int[nodes*size];
	memset(buffer, 0, sizeof(int)*nodes*size);
	cdb3.giveSelection(buffer, size);
	cdb1.giveSelection(buffer+size, size);
	cdb2.giveSelection(buffer+2*size, size);

	printArray(buffer, size);
	printArray(buffer+size, size);
	printArray(buffer+2*size, size);

	cdb1.setIncomingBuffer(buffer, size, nodes, 1);

	vector<int> c;
	while (cdb1.getNextIncomingClause(c)) {
		printVector(c);
	}
	puts("VIP:");
	while (cdb1.getNextIncomingVIPClause(c)) {
		printVector(c);
	}
}

void testClauseDatabaseRandom(int tests) {
	srand(9001);
	int vars = 1000;
	for (int test = 0; test < tests; test++) {
		int nodes = 1 + (rand() % 7);
		int size = 18 + (rand() % 40);
		vector<vector<int> > vips;
		vector<vector<int> > clss;
		ClauseDatabase* dbs = new ClauseDatabase[nodes];
		int* buffer = new int[nodes*size];
		for (int node = 0; node < nodes; node++) {
			int vipc = rand() % 4;
			for (int j = 0; j < vipc; j++) {
				vector<int> cls = makeRandomCls(1+(rand()%4), vars);
				//printVector(cls);
				dbs[node].addVIPClause(cls);
				vips.push_back(cls);
			}
			int ocls = rand() % 20;
			for (int j = 0; j < ocls; j++) {
				vector<int> cls = makeRandomCls(1+(rand()%5), vars);
				//printVector(cls);
				dbs[node].addClause(cls);
				clss.push_back(cls);
			}
			dbs[node].giveSelection(buffer + node*size, size);
			//printArray(buffer, nodes*size);
		}
		int thisNode = rand() % nodes;
		dbs[thisNode].setIncomingBuffer(buffer, size, nodes, thisNode);
		size_t vipsFound = 0;
		vector<int> c;
		while (dbs[thisNode].getNextIncomingVIPClause(c)) {
			//printVector(*c);
			vipsFound++;
			if (find(vips.begin(), vips.end(), c) == vips.end()) {
				printf("Error at test %d, nonexistent vip clause returned.\n", test);
				return;
			}
			if (find(c.begin(), c.end(), 0) != c.end()) {
				printVector(c);
				printf("Error at test %d, clause contains zero(s).\n", test);
				return;
			}
		}
		if (vipsFound != vips.size()) {
			printf("Error at test %d, not all vip clauses returned.\n", test);
			return;
		}
		while (dbs[thisNode].getNextIncomingClause(c)) {
			//printVector(*c);
			if (find(clss.begin(), clss.end(), c) == clss.end()) {
				printf("Error at test %d, nonexistent learned clause returned.\n", test);
				return;
			}
			if (find(c.begin(), c.end(), 0) != c.end()) {
				printVector(c);
				printf("Error at test %d, clause contains zero(s).\n", test);
				return;
			}
		}
		printf("Test %8d (%d nodes, %d size) OK.\n", test, nodes, size);
	}
}


void dataTest() {
	int buff[1500] = {0,0,0,127,-77,-25,-28,-122,-40,-19,-202,-238,111,-233,-256,9,212,130,7,190,-245,-99,-53,-36,-87,-41,-159,202,-27,145,-160,37,205,-40,-251,-208,148,12,144,-241,264,138,-59,39,-234,-231,112,206,-223,-234,56,-231,-36,-87,-53,69,-258,-158,162,50,-195,93,17,-63,190,-245,-99,1,191,137,-27,5,-160,225,-249,48,-131,42,119,69,-258,-158,-150,-262,96,-218,-116,-19,-218,-121,-250,-176,-55,161,128,-207,-146,264,-59,138,39,-234,-231,-207,128,-124,128,-207,234,-243,249,106,-131,119,42,-118,60,-111,162,-195,50,110,-21,-262,-233,9,-256,-163,-20,-90,-181,21,-75,-163,-47,-221,56,-234,11,-19,-193,133,159,156,72,-27,5,-160,-105,-52,135,93,-63,17,56,67,-51,-163,-20,-90,-224,189,-197,-150,-213,-209,-224,189,-197,105,-205,-11,-214,201,75,1,137,191,-28,-38,30,-73,-11,26,-169,-146,128,247,166,119,-236,207,-142,37,-40,205,-233,9,-256,-202,111,-238,-76,-52,139,-28,-38,30,212,130,-262,-150,96,-262,110,-21,-262,-118,-111,60,-196,-220,191,264,138,-59,259,-152,142,69,-258,-158,41,199,-101,93,17,-63,56,-234,11,-263,-182,11,-251,148,-208,-19,-193,133,-208,171,148,-214,201,75,116,169,148,169,54,148,39,-231,-234,-218,-116,-19,105,-205,-11,176,215,122,169,54,148,44,-248,238,73,140,230,-234,56,-231,-224,-197,189,-218,-250,-121,-231,-40,41,-259,17,243,148,-208,225,247,166,119,169,54,148,93,-63,17,-122,-40,-19,-214,201,75,-107,-33,221,-131,42,119,-73,-11,26,106,240,-218,39,-231,-234,-150,-209,-213,-181,21,-75,-259,243,17,105,-205,-11,264,138,-59,-234,-231,56,12,-241,144,1,137,191,93,-63,17,-259,243,17,-231,41,-133,110,-262,-21,-95,161,-136,-243,106,249,-176,-55,161,-224,-197,189,-76,139,-52,56,-234,11,0,0,0,249,219,258,-90,-158,-120,-11,221,-90,-63,127,-257,165,-32,12,239,-209,93,12,-90,17,20,-30,-228,-72,228,-255,-203,-177,174,61,-156,-26,-267,231,-76,-257,63,-199,141,176,45,-220,194,148,-55,76,-90,256,-122,23,-19,-257,255,-158,-109,196,82,118,115,-257,147,114,-33,118,163,238,27,26,89,-166,-136,-163,119,132,-203,143,-15,215,216,193,184,-163,-189,220,17,77,-105,125,-109,-132,247,153,160,75,-171,148,197,-93,-81,41,-71,189,-13,-257,-84,69,178,-90,-163,-155,-109,-90,210,-107,-82,-26,-221,-39,-12,-130,-261,-70,-166,55,99,100,-221,-35,-124,226,-150,221,-231,-212,-167,41,-124,-28,138,-146,-19,73,-193,-122,-257,23,-122,-111,265,-174,-18,-13,-57,154,75,-13,148,160,75,-208,106,-177,59,-10,41,-184,-71,-183,9,-82,-253,130,-201,22,109,76,-68,127,-111,189,-163,-9,48,-20,17,24,-44,-160,-91,174,-92,-99,-165,107,-160,-210,-238,-180,-158,111,17,77,-165,-160,-8,-246,-38,43,-148,-20,35,-185,-80,238,123,239,-9,-133,146,90,146,-23,-90,-35,42,-40,-208,-56,-98,-56,-113,-160,116,-106,40,148,119,79,236,132,-117,112,-115,-230,134,-17,-55,122,208,186,90,130,-253,-186,117,62,156,-256,-115,159,24,-26,-195,13,-175,-134,-108,-139,-2,-166,26,-197,232,-247,-239,219,-211,-212,-230,217,13,92,81,-221,-63,165,-144,168,109,152,13,-187,114,-186,38,-28,24,-189,133,-165,158,34,-45,-18,210,92,122,-239,-148,-20,-68,35,-143,-144,72,159,-105,-70,-50,144,99,100,-35,-221,-261,55,-221,-162,56,59,-124,-234,-146,-2,-195,158,-224,-71,-148,-221,197,210,-212,-19,-92,-157,-24,-55,-227,68,-55,-100,43,-187,-8,-23,-112,-83,107,65,104,-143,-59,-90,232,106,121,219,119,-203,132,143,162,-124,50,155,-219,58,-181,143,-208,91,171,-50,128,107,-83,-236,-190,107,-63,-50,17,24,-44,-160,-157,-177,170,-92,-142,-146,-236,141,93,17,-236,-109,65,167,-162,111,-63,-193,-257,-44,-63,-257,-44,-225,176,-204,109,122,-75,110,-21,-248,82,76,118,-99,-202,-231,-258,-44,180,20,124,-145,-108,41,-258,8,100,-76,41,-82,-143,166,72,96,-120,1,-26,8,210,-107,-82,-26,-163,-47,-8,-225,195,-163,-8,-225,-238,28,-158,-180,26,-181,-99,-245,-38,-107,26,-8,163,17,238,245,211,17,180,122,145,-257,-48,96,174,-26,61,-156,89,-163,-136,-166,132,-123,71,-21,-107,194,-232,221,135,-269,160,-105,-223,152,-143,-241,-133,56,41,-189,238,247,-191,133,202,-231,-158,-159,-163,-155,-109,-90,145,-257,65,96,-248,130,146,24,143,48,158,-21,72,122,-21,96,146,-245,-90,-35,-261,-221,-162,55,99,-225,247,-221,-99,163,238,-147,-117,-230,112,-115,-256,-136,228,117,117,-256,-136,20,160,75,-225,-171,-257,-171,-124,-146,-12,-39,-221,83,89,-44,-134,-208,-158,258,-90,-157,-59,-33,8,-36,-212,-75,-82,115,238,-103,188,44,-68,189,-111,127,-124,-28,138,-146,159,170,163,157,-112,186,-26,-124,-91,76,194,66,-135,144,-221,-213,-140,-148,-68,-20,119,132,79,236,264,-225,204,-59,55,-221,-162,-261,-120,221,-11,-90,109,16,-208,121,148,75,160,-208,-219,-239,63,143,255,-109,-158,196,-132,185,111,55,-100,256,-51,-253,-153,-223,-208,-143,-124,27,-150,221,148,-138,160,-208,230,26,-189,-93,112,12,-13,-30,157,159,-196,-47,115,-212,-75,63,-256,-136,228,117,-209,-82,-150,145,117,-136,-256,20,184,-189,220,-163,-158,69,-90,-157,160,189,-181,138,219,258,-158,-90,-17,-157,-177,134,17,-105,125,77,122,-72,-208,63,20,-30,-228,-72,-238,30,-202,-72,-212,158,115,-248,115,-212,-75,63,-175,-160,-108,239,189,270,-208,-122,-32,-209,239,12,144,-105,-122,99,-163,-153,-221,-208,176,38,175,-148,-8,-81,-30,-160,-189,33,241,-133,-163,211,244,-212,-165,-210,-160,107,5,-82,41,175,41,-183,-184,-71,-148,-68,-20,35,-201,-75,-59,-30,-143,-156,72,-144,112,12,-13,-30,-63,214,-225,-257,184,63,-239,143,-234,184,-122,-212,197,230,59,-13,-107,-234,-92,221,169,200,120,-258,157,244,143,190,190,162,-119,90,-157,211,-24,-92,-231,-44,-205,-133,232,-247,219,-239,-152,-37,-68,-47,134,-17,-177,-47,-148,-85,-68,35,-225,52,-47,197,-187,195,142,80,-63,-144,-225,-44,231,183,11,221,-113,154,61,-31,130,212,9,-253,93,12,-90,17,163,245,17,238,121,153,-93,-122,-55,76,-90,256,-19,73,-193,-122,-190,-50,-63,107,104,-59,-143,-90,-148,-109,131,141,-236,219,233,-125,-59,8,-36,-33,17,-44,24,-160,-43,24,-109,190,157,244,143,190,-154,187,205,90,-177,-157,-22,220,-195,11,193,-153,206,190,-26,30,252,157,-236,63,24,-165,133,-189,-8,-195,224,-26,47,-77,-143,-208,-163,-155,-109,-90,-69,-231,-159,-44,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	ClauseDatabase cdb;
	cdb.setIncomingBuffer(buff, 1500, 1, 5);
	vector<int> c;
	while (cdb.getNextIncomingClause(c)) {
		printVector(c);
	}
}

void memoutTest() {
	int* pts[20];
	size_t start = 1024*1024;
	for (int i = 0; i < 20; i++) {
		pts[i] = (int*) malloc(start*sizeof(int));
		printf("hello %d %lu %p\n", i, start, pts[i]);
		pts[i][0] = 7;
		start *=2;
	}
}

void testClauseFilter(int varsCount, int clausesCount) {
	srand(2015);
	//generate the clauses
	vector<vector<int> > clauses;
	set<string> sclauses;
	for (int i = 0; i < clausesCount; i++) {
		int len = 3 + rand() % 7;
		vector<int> cls = makeRandomCls(len, varsCount);
		clauses.push_back(cls);
		sort(++cls.begin(), cls.end());
		string sclause;
		for (size_t j = 1; j < cls.size(); j++) {
			stringstream ss;
			ss << cls[j];
			string str = ss.str();
			sclause += str + ",";
		}
		sclauses.insert(sclause);
	}
	log(0, "true collisions %d clauses\n", clauses.size() - sclauses.size());

	log(0, "starting adding %d clauses\n", clausesCount);
	double now = getTime();
	ClauseFilter cf;
	int collisions = 0;
	for (size_t i = 0; i < clauses.size(); i++) {
		bool res = cf.registerClause(clauses[i]);
		if (!res) collisions++;
	}
	log(0, "finished adding %d clauses after %.3f secs, collisions: %d\n", clausesCount,
			getTime() - now, collisions);
}

void testBitSet() {
	int* arr1 = new int[10];
	int* arr2 = new int[10];
	memset(arr1, 0, sizeof(int)*10);
	memset(arr2, 0, sizeof(int)*10);

	setBit(arr1, 30);
	setBit(arr1, 31);
	setBit(arr1, 32);
	setBit(arr1, 33);
	setBit(arr1, 62);
	setBit(arr1, 63);
	setBit(arr1, 64);
	setBit(arr1, 65);

	//unsetBit(arr1, 32);

	printf("%d\n", testBit(arr1,30));
	printf("%d\n", testBit(arr1,31));
	printf("%d\n", testBit(arr1,32));
	printf("%d\n", testBit(arr1,33));
	printf("%d\n", testBit(arr1,34));

	//setBit(arr2, 11);
	setBit(arr2, 31);
	setBit(arr2, 32);
	setBit(arr2, 33);
	setBit(arr2, 62);
	//setBit(arr2, 63);
	//setBit(arr2, 64);
	//setBit(arr2, 300);

	printf("%lu\n", countDiffs(arr1, arr2, 10));

	delete[] arr1;
	delete[] arr2;
}

void testBufferManager() {
	BufferManager bf;
	setVerbosityLevel(100);
	int* b1 = bf.getBuffer(100);
	int* b2 = bf.getBuffer(150);
	int* b3 = bf.getBuffer(200);
	bf.returnBuffer(b1);
	bf.returnBuffer(b2);
	//b2 = bf.getBuffer(200);
	bf.returnBuffer(b3);
	//b3 = bf.getBuffer(100);
	//int* b4 = bf.getBuffer(200);
	//bf.cleanReturnedBuffers();
	b1 = bf.getBuffer(100);
	b2 = bf.getBuffer(100);
}

void testClauseManager() {
	int SIG_SIZE = 10;
	int CLS_BUF_SIZE = 30;
	ClauseManager cm1(2, 5, SIG_SIZE, CLS_BUF_SIZE);
	ClauseManager cm2(2, 5, SIG_SIZE, CLS_BUF_SIZE);
	BufferManager bf;
	int* sigbuff1 = bf.getBuffer(SIG_SIZE);
	int* clsbuff1 = bf.getBuffer(CLS_BUF_SIZE);
	int* sigbuff2 = bf.getBuffer(SIG_SIZE);
	int* clsbuff2 = bf.getBuffer(CLS_BUF_SIZE);

	cm1.addClause(makecls(1,2,3));
	cm1.addClause(makecls(4,5,6));

	cm2.addClause(makecls(1,-2,-3));
	cm2.addClause(makecls(4,-5,-6));

	puts("-----------");
	cm1.getSignature(sigbuff1);
	cm2.getSignature(sigbuff2);
	printArray(sigbuff1, SIG_SIZE);
	printArray(sigbuff2, SIG_SIZE);
	puts("-----------");

	cm1.filterHot(clsbuff1, sigbuff2);
	puts("-----------");
	printArray(clsbuff1, CLS_BUF_SIZE);
	puts("-----------");
	vector<vector<int> > cls;
	puts("-----------");
	//cm2.importClauses(clsbuff1, cls);


}

int main(int argc, char **argv) {
	unsigned int s = 4;
	int x = (-1/s)*s;
	printf("%d\n", x);
	cout << "hello world" << "\n";
	//testClauseDatabaseRandom(10000);
	//testClauseDatabaseManual();
	//memoutTest();
	//dataTest();
	//testClauseFilter(10000, 256000);
	//testBufferManager();
	testBitSet();
	//testClauseManager();
}



