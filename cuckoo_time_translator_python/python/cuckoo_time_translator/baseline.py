from __future__ import print_function

import numpy as np
import math
import sys

from timestamp_filters import *
from tools import *

class BaseLine:
    def __init__(self):
        pass

class FilterBaseLine(BaseLine):
  def __init__(self, filter):
    self.filter = filter
    
  def compute(self, raw_hw_times, receive_times):
    base_times = np.array(self.filter.apply(raw_hw_times, receive_times))
    info("Baseline filter after filtering: " + self.filter.getConfigAndStateString())
    return base_times

class FilterBaseLine(BaseLine):
  def __init__(self, filter):
    self.filter = filter
    
  def compute(self, raw_hw_times, receive_times):
    base_times = np.array(self.filter.apply(raw_hw_times, receive_times))
    info("Baseline filter after filtering: " + self.filter.getConfigAndStateString())
    return base_times

class IndexBaseLine(BaseLine):
  def compute(self, raw_hw_times, receive_times):
    return np.linspace(receive_times[0], receive_times[-1], len(receive_times))


class TopicBaseline(BaseLine):
  def __init__(self, bagFile, topic):
    from device_time_bags import readTimestamps
    self.series = readTimestamps(bagFile, topic)
    
    assert len(self.series) > 0
    
  def compute(self, raw_hw_times, receive_times):
    from batch_algo import findGapsAndAffineLinearFit, findBestShiftedModel, IndexShifter, ShiftedModel

    useHwTimeForAssignment = False
    
    xI = raw_hw_times if useHwTimeForAssignment else receive_times
    yI = self.series
    fixSlope = not useHwTimeForAssignment

    bestShiftedModel = findBestShiftedModel(xI, yI, fixSlope)
    
    if useHwTimeForAssignment:
      indexShifter = bestShiftedModel.indexShifter
      x = indexShifter.calcShiftedX(receive_times)
      y = indexShifter.calcShiftedY(yI)
      if len(x) < len(y):
        y = y[:len(x)]

      model = findGapsAndAffineLinearFit(x, y, fixSlope = fixSlope)
      bestShiftedModel = ShiftedModel(indexShifter, model)

    return bestShiftedModel.calcCorrespondingY(range(len(receive_times)), yI)

def create(configString, bagFile):
    name = configString
    baselineFilter = None
    if name == "Index":
      return IndexBaseLine()
    elif name == "LeastSquares":
      baselineFilter = LeastSquaresFilter()
    elif name == "ConvexHull":
      baselineFilter = ConvexHullFilter(True)
    elif name and name[0] == '/':
      return TopicBaseline(bagFile, name)
    else:
      error("Unknown base line method : " + str(name))
      sys.exit(1)

    if baselineFilter:
      return FilterBaseLine(baselineFilter)
