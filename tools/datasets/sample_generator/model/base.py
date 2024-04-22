import abc
import os
from optimum.exporters.onnx import main_export
from ..utils import download


class BaseModel(abc.ABC):
    @property
    @abc.abstractmethod
    def model_id(self):
        pass

    @staticmethod
    @abc.abstractmethod
    def name():
        pass

    @property
    def task(self):
        return "auto"

    @abc.abstractmethod
    def get_model(self, folder, force_download):
        pass

    @abc.abstractmethod
    def preprocess(self, *args, **kwargs):
        pass

    def is_decoder(self):
        return False


class DecoderModel(BaseModel):
    def is_decoder(self):
        return True

    @abc.abstractmethod
    def decode_step(self, *args, **kwargs):
        pass


class SingleModelDownloadMixin(object):
    def get_model(self, output_folder, force_download=False):
        filepath = f"{output_folder}/model.onnx"
        if force_download or not os.path.isfile(filepath):
            print(f"Download model from {self.model_id} to {filepath}")
            download(self.model_id, filepath)
        return filepath


class OptimumHFModelDownloadMixin(object):
    def get_model(self, folder, force_download=False):
        filepath = f"{folder}/model.onnx"
        if force_download or not os.path.isfile(filepath):
            print(f"Download model from {self.model_id} to {filepath}")
            main_export(
                self.model_id,
                output=folder,
                monolith=True,  # forces export into one model
                task=self.task)
        return filepath
