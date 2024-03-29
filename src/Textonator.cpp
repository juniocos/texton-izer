#include "Textonator.h"
#include "ColorUtils.h"
#include "defs.h"

Textonator::Textonator(IplImage * Img, int nClusters, int nMinTextonSize, CvScalar& backgroundPixel):
m_pImg(Img),m_nClusters(nClusters),m_nMinTextonSize(nMinTextonSize),m_backgroundPixel(backgroundPixel)
{
	m_pOutImg = cvCreateImage(cvSize(m_pImg->width,m_pImg->height),
								m_pImg->depth,
								m_pImg->nChannels);
	m_pSmoothImg = cvCreateImage(cvSize(m_pImg->width,m_pImg->height),
		m_pImg->depth,
		m_pImg->nChannels);
	m_pClusters = cvCreateMat( (m_pImg->height * m_pImg->width), 1, CV_32SC1 );
	
	m_pSegmentBoundaries = cvCreateImage(cvGetSize(m_pOutImg), IPL_DEPTH_8U, 1);
  
	m_bgColor = TEXTON_BG_COLOR;

	printf("Textonator Parameters: \n\tMinimal Texton Size=%d, Clusters Number=%d\n", 
										m_nMinTextonSize, 
										m_nClusters);
	if(backgroundPixel.val[0] != UNDEFINED && backgroundPixel.val[1] != UNDEFINED) {
		printf("\tbackground pixel = (%lf,%lf)\n",
			m_backgroundPixel.val[0], m_backgroundPixel.val[1]);
	}

	m_pUnifiedTextonMap = new int[m_pOutImg->height * m_pOutImg->width];
}

Textonator::~Textonator()
{
	cvReleaseMat(&m_pClusters);
	cvReleaseImage(&m_pOutImg);
	cvReleaseImage(&m_pSegmentBoundaries);
}

void Textonator::blurImage()
{
	//gaussian blur (5x5) the image, to smooth out the texture while preserving edge information
	cvSmooth(m_pImg, m_pSmoothImg, CV_BLUR);
}

void Textonator::unifyTextonMaps(vector<int*> & pTextonMapList)
{
	for (int x = 0; x < m_pOutImg->width; x++)
		for (int y = 0; y < m_pOutImg->height; y++)
			m_pUnifiedTextonMap[y * m_pOutImg->width + x] = UNCLUSTERED_PIXEL;

	for (unsigned int i = 0; i < pTextonMapList.size(); i++) {
		for (int x = 0; x < m_pOutImg->width; x++) {
			for (int y = 0; y < m_pOutImg->height; y++) {
				if (pTextonMapList[i][y * m_pOutImg->width + x] >= FIRST_TEXTON_NUM)
					m_pUnifiedTextonMap[y * m_pOutImg->width + x] = i;
			}
		}
	}
}

void Textonator::textonize(vector<Cluster>& clusterList)
{
	vector<int*> pTextonMapList;

	//blur the edge, to remove insignificant edges
	blurImage();

	//we'll start by segmenting and clustering the image
	segment();

	printf("<<< Texton Extraction >>>\n");
	for (int i = 0;i < m_nClusters; i++) {
		int * pTextonMap = new int[m_pOutImg->height * m_pOutImg->width];

		//color the cluster we are currently working on
		colorCluster(i);

		//retrieve the canny edges of the cluster
		cannyEdgeDetect();

		//Extract the textons from the cluster 
		//according to the collected boundaries
		extractTextons(i, clusterList, pTextonMap);

		pTextonMapList.push_back(pTextonMap);
	}

	unifyTextonMaps(pTextonMapList);

	printf("\n>>> Texton Extraction phase completed successfully! <<<\n\n");
}

void Textonator::segment()
{
  printf("\n<<< Feature Extraction >>>\n");

  CFeatureExtraction *pFeatureExtractor = new CFeatureExtraction(m_pSmoothImg);
  pFeatureExtractor->run();

  cluster(pFeatureExtractor);

  delete pFeatureExtractor;
}

void Textonator::cluster(CFeatureExtraction *pFeatureExtractor) 
{
  CvMat * pChannels = 
	  cvCreateMat(pFeatureExtractor->GetPrincipalChannels()->rows,
					pFeatureExtractor->GetPrincipalChannels()->cols,
					pFeatureExtractor->GetPrincipalChannels()->type);

  //normalize the principal channels
  double c_norm = cvNorm(pFeatureExtractor->GetPrincipalChannels(), 0, CV_C, 0);
  cvConvertScale(pFeatureExtractor->GetPrincipalChannels(), pChannels, 1/c_norm);

  //perform k0means on the normalized channels
  cvKMeans2(pChannels, m_nClusters, m_pClusters,  
	  cvTermCriteria( CV_TERMCRIT_EPS+CV_TERMCRIT_ITER, 100, 0.001 ));
    
  cvReleaseMat(&pChannels);
}

void Textonator::colorCluster(int nCluster)
{
  uchar * pData  = (uchar *)m_pOutImg->imageData;
  cvCvtColor(m_pImg, m_pOutImg, CV_BGR2YCrCb);
  CvScalar color = cvScalarAll(0);

  for (int i=0; i<m_pImg->height; i++){
      for (int j=0; j<m_pImg->width; j++) {
          if (m_pClusters->data.i[i*m_pImg->width+j] != nCluster) {
            ColorUtils::recolorPixel(pData, i,j, m_pImg->widthStep, &color);
          }
	  }
  }

  cvCvtColor(m_pOutImg, m_pOutImg, CV_YCrCb2BGR);
}

void Textonator::cannyEdgeDetect()
{
	IplImage * bn = cvCreateImage(cvGetSize(m_pOutImg), IPL_DEPTH_8U, 1);

	cvCvtColor(m_pOutImg, bn, CV_BGR2GRAY);

	cvCanny(bn, m_pSegmentBoundaries, 70, 90);

    cvReleaseImage(&bn);
}

void Textonator::assignTextons(int x, 
							   int y, 
							   uchar * pData, 
							   int * pTextonMap, 
							   int nTexton)
{
	//if we are in no man's land or if we are already coloured
	if (pTextonMap[y*m_pImg->width+x] == OUT_OF_SEGMENT_DATA
		|| pTextonMap[y*m_pImg->width+x] >= FIRST_TEXTON_NUM)
		return;

	m_nCurTextonSize++;

	//if we are on a border, color it and return
	if (pTextonMap[y*m_pImg->width+x] == BORDER_DATA) {
		pTextonMap[y*m_pImg->width+x] = nTexton;
		return;
	}

	//color the pixel with the current 
	pTextonMap[y*m_pImg->width+x] = nTexton;

	//spread to the 4-connected neighbors (if possible)
	if ((x < (m_pImg->width - 1)) && 
		(pTextonMap[y*m_pImg->width+x + 1] == UNCLUSTERED_DATA || 
		 pTextonMap[y*m_pImg->width+x + 1] == BORDER_DATA))
	{
		assignTextons(x+1,y,pData,pTextonMap,nTexton);
	}
	if ((x >= 1) && 
		(pTextonMap[y*m_pImg->width+x - 1] == UNCLUSTERED_DATA || 
		 pTextonMap[y*m_pImg->width+x - 1] == BORDER_DATA)) 
	{
		assignTextons(x-1,y,pData,pTextonMap,nTexton);
	}
	if ((y < (m_pImg->height - 1)) && 
		(pTextonMap[(y + 1)*m_pImg->width+x] == UNCLUSTERED_DATA || 
		pTextonMap[(y + 1)*m_pImg->width+x] == BORDER_DATA)) 
	{
		assignTextons(x,y+1,pData,pTextonMap,nTexton);
	}
	if ((y >= 1) && 
		(pTextonMap[(y - 1)*m_pImg->width+x] == UNCLUSTERED_DATA || 
		 pTextonMap[(y - 1)*m_pImg->width+x] == BORDER_DATA))
	{
		assignTextons(x,y-1,pData,pTextonMap,nTexton);
	}
}

void Textonator::colorTextonMap(uchar *pBorderData, int * pTextonMap, int nCluster)
{
	int borderStep = m_pSegmentBoundaries->widthStep;
	for (int i=0; i < m_pOutImg->width; i++){
      for (int j=0; j < m_pOutImg->height; j++) {
		  if (m_pClusters->data.i[j*m_pImg->width+i] == nCluster)
		  {
				if (pBorderData[j*borderStep + i] == EDGE_DATA)
					pTextonMap[j * m_pOutImg->width + i] = BORDER_DATA;
				else
					pTextonMap[j * m_pOutImg->width + i] = UNCLUSTERED_DATA;
		  }
		  else
				pTextonMap[j * m_pOutImg->width + i] = OUT_OF_SEGMENT_DATA;
	  }
  }
}

int Textonator::scanForTextons(int nCluster, bool &fBackgroundCluster, int * pTextonMap)
{
	uchar * pData  = (uchar *) m_pOutImg->imageData;
	uchar * pBorderData  = (uchar *) m_pSegmentBoundaries->imageData;

	// Create a texton map with the values:
	//		BORDER_DATA, UNCLUSTERED_DATA, OUT_OF_SEGMENT_DATA
	// For the cluster nCluster
	colorTextonMap(pBorderData, pTextonMap, nCluster);

	int nTexton = FIRST_TEXTON_NUM;

	//Check if its a background cluster
	if (m_backgroundPixel.val[0] != UNDEFINED && 
		m_backgroundPixel.val[1] != UNDEFINED){
		int pos = (int)m_backgroundPixel.val[1]*m_pOutImg->width+(int)m_backgroundPixel.val[0];
		if (pTextonMap[pos] == UNCLUSTERED_DATA || pTextonMap[pos] == BORDER_DATA)
			fBackgroundCluster = true;
	}

	// For each pixel perform a flood fill with the value of the current texton
	// Each time we find a texton advance the texton count
	for (int i=0; i < m_pOutImg->width; i++)
	{
		for (int j=0; j < m_pOutImg->height; j++) 
		{
			//a texton that has not been clustered
			if (pTextonMap[j*m_pOutImg->width+i] == UNCLUSTERED_DATA){

				m_nCurTextonSize = 0;

				assignTextons(i,j, pData, pTextonMap, nTexton);
				
				//if (fBackgroundCluster)
				//	continue;

				// Check if our texton is large enough
				if (m_nCurTextonSize > m_nMinTextonSize){
						nTexton++;
					//printf("\t(Texton #%d) i=%d,j=%d, Size=%d\n", 
					//		nTexton - FIRST_TEXTON_NUM, i, j, m_nCurTextonSize);
				}
				else
				{
					//return the colored pixel to the pixel pool
					for (int i=0; i < m_pOutImg->width; i++){
						for (int j=0; j < m_pOutImg->height; j++) {
							if (pTextonMap[j * m_pOutImg->width + i] == nTexton) {
								pTextonMap[j * m_pOutImg->width + i] = UNCLUSTERED_DATA;
							}
						}
					}
				}
			}
		} 
	}

	//if (fBackgroundCluster)
	//	return 1;
	//else
	return (nTexton - FIRST_TEXTON_NUM);
}

void Textonator::retrieveTextons(int nClusterSize, 
								 int nCluster, 
								 bool fBackgroundCluster,
								 int * pTextonMap, 
								 vector<Cluster>& clusterList)
{
	uchar * pData  = (uchar *) m_pOutImg->imageData;
	bool firstBackgroundTexton = fBackgroundCluster;
	int nCurTexton = FIRST_TEXTON_NUM;
	list<Texton*> curTextonList;

	//create the new cluster
	Cluster cluster;

	//add a full background texton to the background cluster
	if (fBackgroundCluster)
		nClusterSize++;

	for (int nNum = 0; nNum < nClusterSize; nNum++){

		//"Replenish" the original image
		memcpy(pData, (uchar *)m_pImg->imageData, m_pImg->imageSize);

		int minX = m_pImg->width;
		int minY = m_pImg->height;
		int maxX = 0;
		int maxY = 0;

		int nCount = 0;
		//figure out the texton dimensions
		for (int i = 0; i < m_pOutImg->width; i++){
			for (int j = 0; j < m_pOutImg->height; j++) {
				bool fCheck;
				if (firstBackgroundTexton)
					fCheck = pTextonMap[j * m_pOutImg->width + i] >= FIRST_TEXTON_NUM;
				else
					fCheck = pTextonMap[j * m_pOutImg->width + i] == nCurTexton;

				if (fCheck)
				{
					if (minX > i) minX = i;
					if (minY > j) minY = j;
					if (maxX < i) maxX = i;
					if (maxY < j) maxY = j;

					nCount++;
				}
				else {
					ColorUtils::recolorPixel(pData, j, i, m_pOutImg->widthStep, &m_bgColor);
				}
			}
		}

		if (firstBackgroundTexton)
			firstBackgroundTexton = false;
		else
			nCurTexton++;

		
		// Create bounding box with the size of the texton
		SBox boundingBox(minX, minY, maxX, maxY);

		int xSize = boundingBox.getWidth();
		int ySize = boundingBox.getHeight();

		// Create an image with the size of the texton
		// Copy the texton to the new image
		IplImage* pTexton = cvCreateImage(cvSize(xSize,ySize), 
											m_pImg->depth,
											m_pImg->nChannels);
		
		extractTexton(boundingBox, pData, pTexton);

		Texton* t = new Texton(pTexton, 
								nCluster, 
								boundingBox.getPositionMask(m_pImg), 
								boundingBox);

		// if the cluster is known as background or is as big as the picture, 
		//it is considered background
		if (firstBackgroundTexton || 
			xSize >= m_pImg->width - 10 || 
			ySize >= m_pImg->height - 10){
			cluster.setImageBackground();
			t->setImageBackground();
		}

		curTextonList.push_back(t);
	}

	//create the cluster and add it to the cluster list
	cluster.m_textonList = curTextonList;
	cluster.m_nClusterSize = nClusterSize;
	clusterList.push_back(cluster);
}

void Textonator::extractTexton(int minX, 
							   int maxX, 
							   int minY, 
							   int maxY, 
							   uchar * pImageData, 
							   IplImage* pTexton)
{
	CvScalar color;
	int step = m_pOutImg->widthStep;
	uchar * pImData  = (uchar *)pTexton->imageData;

	for (int i = minX; i < maxX; i++)
	{
		for (int j = minY; j < maxY; j++) 
		{
			color.val[0] = pImageData[j*step+i*3+0];
			color.val[1] = pImageData[j*step+i*3+1];
			color.val[2] = pImageData[j*step+i*3+2];

			ColorUtils::recolorPixel(pImData, j - minY, i - minX, pTexton->widthStep, &color);
		}
	}
}

void Textonator::extractTexton(SBox& boundingBox, 
							   uchar * pImageData, 
							   IplImage* pTexton)
{
	extractTexton(boundingBox.minX, 
					boundingBox.maxX, 
					boundingBox.minY, 
					boundingBox.maxY, 
					pImageData, 
					pTexton);
}

void Textonator::assignStrayPixels(int * pTextonMap, int nSize)
{
	int nOtherTextons[8];
	int nCurTexton;

	int * pNewTextonMap = new int[nSize];
	memset(pNewTextonMap, UNDEFINED, nSize*sizeof(int));

	for (int i = 0; i < m_pOutImg->width; i++){
		for (int j = 0; j < m_pOutImg->height; j++) {

			//Reset the neighbor pixel associations
			memset(nOtherTextons, UNDEFINED, 8 *sizeof(int));
			nCurTexton = pTextonMap[j * m_pOutImg->width + i];

			//we check only out of segment data
			if (nCurTexton != OUT_OF_SEGMENT_DATA) {
				pNewTextonMap[j * m_pOutImg->width + i] = pTextonMap[j * m_pOutImg->width + i];
				continue;
			}

			getNeighbors(pTextonMap, i, j, m_pOutImg->width, m_pOutImg->height, nOtherTextons);

			int nMatchNum = 0;
			int nCurMatchNum;
			int nTextonNum = -1;
			for (int k = 0; k < 8; k++) {
				nCurMatchNum = 0;
				for (int l = 0; l < 8; l++){
					if (nOtherTextons[k] == nOtherTextons[l])
						nCurMatchNum++;
				}
				if (nCurMatchNum >= 7 && nTextonNum == -1)
					nTextonNum = nOtherTextons[k];
				nMatchNum += nCurMatchNum;
			}

			//if there are more than 7 matched neighbors
			if (nTextonNum >= FIRST_TEXTON_NUM && nMatchNum >= 7*7){
				pNewTextonMap[j * m_pOutImg->width + i] = nTextonNum;
			}
		}
	}

	memcpy(pTextonMap, pNewTextonMap, nSize * sizeof(int));

	delete [] pNewTextonMap;
}

void Textonator::getNeighbors(int *map, int i, int j, int width, int height, int arrNeighbors[]) 
{
	if (i > 1){
		arrNeighbors[0] = map[j * width + i - 1];
		if (j < height - 1)
			arrNeighbors[5] = map[(j+1) * width + i - 1];
	}

	if (i < width - 1){
		arrNeighbors[1] = map[j * width + i + 1];
		if (j < height - 1)
			arrNeighbors[4] = map[(j+1) * width + i + 1];
	}

	if (j > 1) {
		arrNeighbors[2] = map[(j-1) * width + i];
		if (i < width - 1)
			arrNeighbors[6] = map[(j-1) * width + i + 1];
		if (i > 1)
			arrNeighbors[7] = map[(j-1) * width + i - 1];
	}

	if (j < height - 1)
		arrNeighbors[3] = map[(j+1) * width + i];
}

void Textonator::assignRemainingData(int * pTextonMap)
{
	int nChanges;
	int nCurTexton;

	// 8 textons surrounding the current texton
	int nOtherTextons[8];

	do {
		nChanges = 0;

		for (int i = 0; i < m_pOutImg->width; i++){
			for (int j = 0; j < m_pOutImg->height; j++) {

				//Reset the neighbor pixel associations
				memset(nOtherTextons, UNDEFINED, 8 *sizeof(int));

				nCurTexton = pTextonMap[j * m_pOutImg->width + i];

				//we check only unclustered data
				if (nCurTexton != UNCLUSTERED_DATA)
					continue;

				getNeighbors(pTextonMap, i, j, m_pOutImg->width, m_pOutImg->height, nOtherTextons);

				bool fPaint = false;
				int nClosestTextonColor = UNDEFINED;
				for (int k = 0; k < 8; k++) {
					if (nOtherTextons[k] == UNDEFINED)
						continue;

					//If there is only texton near the unassigned pixel, become part of it 
					if (nOtherTextons[k] >= FIRST_TEXTON_NUM) {
						if (nClosestTextonColor == UNDEFINED){
							nClosestTextonColor = nOtherTextons[k];
							fPaint = true;
						}
						else if (nClosestTextonColor != nOtherTextons[k]) {
							fPaint = false;
						}
					}
				}

				if (fPaint){
					pTextonMap[j * m_pOutImg->width + i] = nClosestTextonColor;
					nChanges++;
				}
			}
		}
	} while (nChanges != 0);
}

void Textonator::extractTextons(int nCluster, vector<Cluster>& clusterList, int * pTextonMap)
{
	bool fBackgroundCluster = false;
	printf("* Extracting textons from cluster #%d...\n", nCluster);

	//initialize the texton map
	int nSize = m_pOutImg->height * m_pOutImg->width;
	memset(pTextonMap, 0, nSize*sizeof(int));

	// Extract textons from cluster
	int nClusterSize = scanForTextons(nCluster, fBackgroundCluster, pTextonMap);

	//assign all the remaining untextoned pixels the closest texton
	assignRemainingData(pTextonMap);

	//assign any lonely pixels that may appear inside a texton and belong to another cluster to that texton
	assignStrayPixels(pTextonMap, nSize);

	retrieveTextons(nClusterSize, nCluster, fBackgroundCluster, pTextonMap, clusterList);
}

void Textonator::retrieveTextonCoOccurences(int nCluster, int nOffsetCurTexton, vector<Occurence>& Occurences, CvScalar& bg, uchar * pData,vector<int*> pTextonMapList, vector<Cluster>& clusterList)
{
	int nDilationSize = 1;
	int nDilation = 0;
	int nMaxDilations = MAX_DILATIONS;

	Occurences.clear();

	while (nDilation < nMaxDilations) {
		cvDilate(m_pOutImg, m_pOutImg, NULL, nDilationSize);

		for (int i = 0; i < m_pOutImg->width; i++){
			for (int j = 0; j < m_pOutImg->height; j++) {
				int pos = j*m_pOutImg->widthStep+i*3;
				CvScalar color = cvScalar(pData[pos],pData[pos+1],
										pData[pos+2]);
				if (!ColorUtils::compareColors(color, bg)){
						//search through the clusters for overlapping textons
						for (int nCurrentCluster = 0; nCurrentCluster < m_nClusters; nCurrentCluster++){
							int nCollidingTexton = pTextonMapList[nCurrentCluster][j * m_pOutImg->width + i];
							if (clusterList[nCurrentCluster].isImageBackground())
								continue;

							if (nCollidingTexton < FIRST_TEXTON_NUM)
								continue;

							if (nCollidingTexton != nOffsetCurTexton ||
								nCollidingTexton == nOffsetCurTexton && nCurrentCluster != nCluster){

									//Retrieve texton
									list<Texton*>::iterator iter = clusterList[nCurrentCluster].m_textonList.begin(); 
									for (int p = 0; p < nCollidingTexton - FIRST_TEXTON_NUM; p++)
										iter++;

									Texton * t = *(iter);

									if (t->isImageBackground())
										continue;

									bool isInList = false;
	
									for (unsigned int c = 0; c < Occurences.size(); c++){
										if (Occurences[c].m_nTexton == nCollidingTexton
										&& Occurences[c].m_nCluster == nCurrentCluster)
											isInList = true;
									}

									if (isInList == false){
										//remember the size of the area where there is no texton intersection
										if (Occurences.size() == 0){
											//from the moment we occur another texton, we will dilate oncee EXTRA_DILATIONS
											nMaxDilations = nDilation + nDilationSize + 1;
											nDilationSize = EXTRA_DILATIONS;
										}

										Occurence oc( 
											nCollidingTexton,
											nCurrentCluster,
											nDilation);
										Occurences.push_back(oc);
									}
							}
						}
				}
			}
		}

		nDilation++;
	}
}

void Textonator::computeTextonCoOccurences(Texton * curTexton, vector<Occurence>& Occurences, vector<Cluster>& clusterList)
{
	vector<CoOccurences> coOccurencesList;

	int nMinDilation = UNDEFINED;
	size_t fSides[4];
	memset(fSides, 0, 4 * sizeof(int));

	for (unsigned int c = 0; c < Occurences.size(); c++) {

		list<Texton*>::iterator iter = clusterList[Occurences[c].m_nCluster].m_textonList.begin(); 
		for (int i = 0; i < Occurences[c].m_nTexton - FIRST_TEXTON_NUM; i++)
			iter++;

		Texton * t = *(iter);

		//if the texton is filling the whole image, then he cannot be trusted as neighbor
		if (t->isImageBackground())
			continue;

		if (nMinDilation == UNDEFINED)
			nMinDilation = Occurences[c].m_nDistance;
		else 
			nMinDilation = MIN(nMinDilation, Occurences[c].m_nDistance);

		//Avoid side textons
		if (t->getPosition() != Texton::NON_BORDER)
			continue;

		SBox orgBox = curTexton->getBoundingBox();
		SBox box = t->getBoundingBox();

		int xDst = 0;
		int yDst = 0;

		//distance will always be from the bottom-right point of the original texton to the
		//up-left point of the current texton
		xDst = box.minX - orgBox.minX;
		yDst = box.minY - orgBox.minY;

		CoOccurences cooccurence(xDst, yDst, Occurences[c].m_nCluster);
		coOccurencesList.push_back(cooccurence);	

		if (xDst >= 0 && yDst >= 0)
			fSides[0] = coOccurencesList.size() - 1;
		if (xDst <= 0 && yDst <= 0)
			fSides[1] = coOccurencesList.size() - 1;
		if (xDst <= 0 && yDst >= 0)
			fSides[2] = coOccurencesList.size() - 1;
		if (xDst >= 0 && yDst <= 0)
			fSides[3] = coOccurencesList.size() - 1;
	}

	curTexton->setDilationArea(nMinDilation + 1);

	//add axes-symmetric textons, in order to ensure full spread
	for (int i = 0; i < 4; i++){
		if (fSides[i] == 0){
			size_t nChosen;
			if (i % 2 == 0)
				nChosen = fSides[i + 1];
			else
				nChosen = fSides[i - 1];

			if (nChosen == 0)
				continue;

			fSides[i] = nChosen;
			CoOccurences cooccurence(-coOccurencesList[nChosen].distX
				, -coOccurencesList[nChosen].distY
				, coOccurencesList[nChosen].nCluster);
			coOccurencesList.push_back(cooccurence);	
		}

	}

	curTexton->setCoOccurences(coOccurencesList);
}

void Textonator::computeCoOccurences(vector<int*> pTextonMapList, vector<Cluster>& clusterList)
{
	vector<Occurence> textonOccurencesList;
	CvScalar bg = cvScalarAll(0);
	CvScalar white_bg = cvScalarAll(255);

	printf("* Computing textons' co-occurrences...");

	uchar * pData  = (uchar *) m_pOutImg->imageData;
	for (unsigned int nCluster = 0; nCluster < pTextonMapList.size(); nCluster++) {
		int nOffsetCurTexton = FIRST_TEXTON_NUM;

		for (list<Texton*>::iterator iter = clusterList[nCluster].m_textonList.begin(); 
			iter != clusterList[nCluster].m_textonList.end(); 
			iter++, nOffsetCurTexton++) {

			Texton * curTexton = (*iter);

			//this texton is probably background as it fills the whole image and cannot really be used
			if (curTexton->isImageBackground())
				continue;

			//"Replenish" the original image
			memcpy(pData, (uchar *)m_pImg->imageData, m_pImg->imageSize);
			for (int i = 0; i < m_pOutImg->width; i++){
				for (int j = 0; j < m_pOutImg->height; j++) {
					if (pTextonMapList[nCluster][j * m_pOutImg->width + i] != nOffsetCurTexton){
						ColorUtils::recolorPixel(pData, j, i, m_pOutImg->widthStep, &bg);
					}
					else {
						//Dilation finds the maximum over a local neighborhood, so let the texton be that maximum
						ColorUtils::recolorPixel(pData, j, i, m_pOutImg->widthStep, &white_bg);
					}
				}
			}
			
			retrieveTextonCoOccurences(nCluster, nOffsetCurTexton, textonOccurencesList, bg, pData,pTextonMapList, clusterList);
			
			//Compute co occurences relations only to non border textons
			if (curTexton->getPosition() == Texton::NON_BORDER) 
				computeTextonCoOccurences(curTexton, textonOccurencesList, clusterList);	
		}
	}
}
