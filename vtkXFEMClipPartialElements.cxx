/*=========================================================================

  Module:    vtkXFEMClipPartialElements.cxx
  Copyright (c) 2017 Battelle Energy Alliance, LLC

  Based on Visualization Toolkit
  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkXFEMClipPartialElements.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkFloatArray.h"
#include "vtkGenericCell.h"
#include "vtkPointData.h"
#include "vtkImplicitFunction.h"
#include "vtkIncrementalPointLocator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMergePoints.h"
#include "vtkObjectFactory.h"
#include "vtkPlane.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"
#include "vtkSmartPointer.h"

vtkStandardNewMacro(vtkXFEMClipPartialElements);

//----------------------------------------------------------------------------
vtkXFEMClipPartialElements::vtkXFEMClipPartialElements()
{
  this->OutputPointsPrecision = DEFAULT_PRECISION;
  this->Locator = NULL;
}

//----------------------------------------------------------------------------
vtkXFEMClipPartialElements::~vtkXFEMClipPartialElements()
{
  if ( this->Locator )
  {
    this->Locator->UnRegister(this);
    this->Locator = NULL;
  }
}

//----------------------------------------------------------------------------
void vtkXFEMClipPartialElements::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}

//----------------------------------------------------------------------------
void vtkXFEMClipPartialElements::CreateDefaultLocator()
{
  if ( this->Locator == NULL )
  {
    this->Locator = vtkMergePoints::New();
    this->Locator->Register(this);
    this->Locator->Delete();
  }
}

//----------------------------------------------------------------------------
int vtkXFEMClipPartialElements::RequestData(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *outputVector)
{
  // get the info objects
  vtkInformation *inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  // get the input and output
  vtkDataSet *realInput = vtkDataSet::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  // We have to create a copy of the input because clip requires being
  // able to InterpolateAllocate point data from the input that is
  // exactly the same as output. If the input arrays and output arrays
  // are different vtkCell3D's Clip will fail. By calling InterpolateAllocate
  // here, we make sure that the output will look exactly like the output
  // (unwanted arrays will be eliminated in InterpolateAllocate). The
  // last argument of InterpolateAllocate makes sure that arrays are shallow
  // copied from realInput to input.
  vtkSmartPointer<vtkDataSet> input;
  input.TakeReference(realInput->NewInstance());
  input->CopyStructure(realInput);
  input->GetCellData()->PassData(realInput->GetCellData());
  input->GetPointData()->InterpolateAllocate(realInput->GetPointData(), 0, 0, 1);

  vtkUnstructuredGrid *output = vtkUnstructuredGrid::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkIdType numPts = input->GetNumberOfPoints();
  vtkIdType numCells = input->GetNumberOfCells();
  vtkPointData *inPD=input->GetPointData(), *outPD = output->GetPointData();
  vtkCellData *inCD=input->GetCellData();
  vtkCellData *outCD = output->GetCellData();
  vtkPoints *newPoints;
  vtkFloatArray *cellScalars;
  vtkImplicitFunction *ClipFunction = NULL;
  vtkPlane *ClipPlane = NULL;

//  vtkDataArray *clipScalars;
  bool cutBySignedDist(false);
  vtkDataArray *XFEMSignedDistArray0 = input->GetCellData()->GetArray("xfem_signed_dist0");
  vtkDataArray *XFEMSignedDistArray1 = input->GetCellData()->GetArray("xfem_signed_dist1");
  vtkDataArray *XFEMSignedDistArray2 = input->GetCellData()->GetArray("xfem_signed_dist2");
  vtkDataArray *XFEMSignedDistArray3 = input->GetCellData()->GetArray("xfem_signed_dist3");
  if (XFEMSignedDistArray0 &&
      XFEMSignedDistArray1 &&
      XFEMSignedDistArray2 &&
      XFEMSignedDistArray3) {
    cutBySignedDist = true;
  }

  bool cutByPlane(false);
  vtkDataArray *XFEMCutOriginArray = input->GetCellData()->GetArray("xfem_cut_origin_");
  vtkDataArray *XFEMCutNormalArray = input->GetCellData()->GetArray("xfem_cut_normal_");
  if (XFEMCutOriginArray &&
      XFEMCutNormalArray) {
    cutByPlane = true;
    ClipFunction = vtkPlane::New();
    ClipPlane = vtkPlane::SafeDownCast(ClipFunction);
  }

  if (cutBySignedDist && cutByPlane)
  {
    std::cout<<"Cannot cut by both signed distance and plane"<<std::endl;
    return 1;
  }
  else if (!cutBySignedDist && !cutByPlane)
  {
    std::cout<<"Must provide either data to cut by signed distance or plane"<<std::endl;
    return 1;
  }
  vtkPoints *cellPts;
  vtkIdList *cellIds;
  double s;
  vtkIdType npts;
  vtkIdType *pts;
  int cellType = 0;
  vtkIdType i;
  int j;
  vtkIdType estimatedSize;
  int numOutputs = 1;

  vtkDebugMacro(<< "Clipping dataset");

  int inputObjectType = input->GetDataObjectType();

  // Initialize self; create output objects
  //
  if ( numPts < 1 )
  {
    vtkDebugMacro(<<"No data to clip");
    return 1;
  }

  // allocate the output and associated helper classes
  estimatedSize = numCells;
  estimatedSize = estimatedSize / 1024 * 1024; //multiple of 1024
  if (estimatedSize < 1024)
  {
    estimatedSize = 1024;
  }
  cellScalars = vtkFloatArray::New();
  cellScalars->Allocate(VTK_CELL_SIZE);
  vtkCellArray *conn = vtkCellArray::New();
  conn->Allocate(estimatedSize,estimatedSize/2);
  conn->InitTraversal();
  vtkUnsignedCharArray *types = vtkUnsignedCharArray::New();
  types->Allocate(estimatedSize,estimatedSize/2);
  vtkIdTypeArray *locs = vtkIdTypeArray::New();
  locs->Allocate(estimatedSize,estimatedSize/2);
  newPoints = vtkPoints::New();

  // set precision for the points in the output
  if(this->OutputPointsPrecision == vtkAlgorithm::DEFAULT_PRECISION)
  {
    vtkPointSet *inputPointSet = vtkPointSet::SafeDownCast(input);
    if(inputPointSet)
    {
      newPoints->SetDataType(inputPointSet->GetPoints()->GetDataType());
    }
  }
  else if(this->OutputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    newPoints->SetDataType(VTK_FLOAT);
  }
  else if(this->OutputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    newPoints->SetDataType(VTK_DOUBLE);
  }

  newPoints->Allocate(numPts,numPts/2);

  // locator used to merge potentially duplicate points
  if ( this->Locator == NULL )
  {
    this->CreateDefaultLocator();
  }
  this->Locator->InitPointInsertion (newPoints, input->GetBounds());

  vtkDataSetAttributes* tempDSA = vtkDataSetAttributes::New();
  tempDSA->InterpolateAllocate(inPD, 1, 2);
  outPD->InterpolateAllocate(inPD,estimatedSize,estimatedSize/2);
  tempDSA->Delete();
  outCD = output->GetCellData();
  outCD->CopyAllocate(inCD,estimatedSize,estimatedSize/2);

  //Process all cells and clip each in turn
  //
  int abort=0;
  vtkIdType updateTime = numCells/20 + 1;  // update roughly every 5%
  vtkGenericCell *cell = vtkGenericCell::New();
  int num = 0;
  int numNew = 0;
  for (vtkIdType cellId=0; cellId < numCells && !abort; cellId++)
  {
    if ( !(cellId % updateTime) )
    {
      this->UpdateProgress(static_cast<double>(cellId) / numCells);
      abort = this->GetAbortExecute();
    }

    input->GetCell(cellId,cell);
    cellPts = cell->GetPoints();
    cellIds = cell->GetPointIds();
    npts = cellPts->GetNumberOfPoints();

    if (cutBySignedDist)
    {
      double NodalSignedDist[4];

      NodalSignedDist[0] = XFEMSignedDistArray0->GetTuple1(cellId);
      NodalSignedDist[1] = XFEMSignedDistArray1->GetTuple1(cellId);
      NodalSignedDist[2] = XFEMSignedDistArray2->GetTuple1(cellId);
      NodalSignedDist[3] = XFEMSignedDistArray3->GetTuple1(cellId);

      for (i=0; i<4; ++i)
      {
        if (NodalSignedDist[i] < 0)
        {
          // Hack to leave a slight gap between the opposing cut
          // planes so that the nodes don't get merged together.
          NodalSignedDist[i] *= 1.0001;
        }
        cellScalars->InsertTuple(i,&NodalSignedDist[i]);
      }
    }
    else if (cutByPlane)
    {
      double *Origin;
      Origin = XFEMCutOriginArray->GetTuple3(cellId);
      ClipPlane->SetOrigin(Origin);

      double *Normal;
      Normal = XFEMCutNormalArray->GetTuple3(cellId);
      double len = Normal[0]*Normal[0] +
                   Normal[1]*Normal[1] +
                   Normal[2]*Normal[2];
      len = sqrt(len);
      double tol = 1.e-15;
      if (len > tol)
      {
        //Normals are output from XFEM code facing outward from cut plane.
        //Needs to face into cut plane for vtk clip plane, so mult by -1.
        Normal[0] = -Normal[0]/len;
        Normal[1] = -Normal[1]/len;
        Normal[2] = -Normal[2]/len;
        ClipPlane->SetNormal(Normal);

        // evaluate plane cutting function
        for ( i=0; i < npts; i++ )
        {
          cellPts = cell->GetPoints();
          s = ClipFunction->FunctionValue(cellPts->GetPoint(i));
          if (s < 0)
          {
            // Hack to leave a slight gap between the opposing cut
            // planes so that the nodes don't get merged together.
            s *= 1.0001;
          }
          cellScalars->InsertTuple(i,&s);
        }
      }
      else //Don't cut this element
      {
        for ( i=0; i < npts; i++ )
        {
          s=1.0;
          cellScalars->InsertTuple(i,&s);
        }
      }
    }

    double value = 0.0;
    int InsideOut = 0;

    // perform the clipping
    cell->Clip(value, cellScalars, this->Locator, conn,
               inPD, outPD, inCD, cellId, outCD, InsideOut);
    numNew = conn->GetNumberOfCells() - num;
    num = conn->GetNumberOfCells();

    for (j=0; j < numNew; j++)
    {
      if (cell->GetCellType() == VTK_POLYHEDRON)
      {
        //Polyhedron cells have a special cell connectivity format
        //(nCell0Faces, nFace0Pts, i, j, k, nFace1Pts, i, j, k, ...).
        //But we don't need to deal with it here. The special case is handled
        //by vtkUnstructuredGrid::SetCells(), which will be called next.
        types->InsertNextValue(VTK_POLYHEDRON);
      }
      else
      {
        locs->InsertNextValue(conn->GetTraversalLocation());
        conn->GetNextCell(npts,pts);

        //For each new cell added, got to set the type of the cell
        switch ( cell->GetCellDimension() )
        {
          case 0: //points are generated--------------------------------
            cellType = (npts > 1 ? VTK_POLY_VERTEX : VTK_VERTEX);
            break;

          case 1: //lines are generated---------------------------------
            cellType = (npts > 2 ? VTK_POLY_LINE : VTK_LINE);
            break;

          case 2: //polygons are generated------------------------------
            cellType = (npts == 3 ? VTK_TRIANGLE :
                        (npts == 4 ? VTK_QUAD : VTK_POLYGON));
            break;

          case 3: //tetrahedra or wedges are generated------------------
            cellType = (npts == 4 ? VTK_TETRA : VTK_WEDGE);
            break;
        } //switch

        types->InsertNextValue(cellType);
      }
    } //for each new cell
  } //for each cell

  cell->Delete();
  cellScalars->Delete();

  if ( ClipFunction)
  {
    ClipFunction->Delete();
  }

  output->SetPoints(newPoints);
  output->SetCells(types, locs, conn);
  conn->Delete();
  types->Delete();
  locs->Delete();

  newPoints->Delete();
  this->Locator->Initialize();//release any extra memory
  output->Squeeze();

  return 1;
}
